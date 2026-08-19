#include <pjlib.h>
#include <pjlib-util.h>

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/printk.h>

#define PHASE3_LIFECYCLES 3
#define PHASE3_INIT_REPEATS 3
#define GUARD_WORD UINT32_C(0x51A7C0DE)
#define GUARD_BYTE UINT8_C(0xA5)

static int scanner_error_count;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 3] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 3] FAIL %s:%d condition=%s\n",
	       test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                     \
		pj_status_t check_status_ = (expression);                           \
		if (check_status_ != PJ_SUCCESS)                                    \
			return fail_status((test_name), __LINE__, check_status_);      \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                     \
		if (!(condition))                                                  \
			return fail_value((test_name), __LINE__, #condition);         \
	} while (0)

static pj_bool_t str_equals(const pj_str_t *value, const char *expected)
{
	pj_size_t expected_len = strlen(expected);

	return value->slen == (pj_ssize_t)expected_len &&
	       memcmp(value->ptr, expected, expected_len) == 0;
}

static pj_bool_t str_starts_with(const pj_str_t *value, const char *prefix)
{
	pj_size_t prefix_len = strlen(prefix);

	return value->slen >= (pj_ssize_t)prefix_len &&
	       memcmp(value->ptr, prefix, prefix_len) == 0;
}

static pj_bool_t bytes_are(const uint8_t *bytes, pj_size_t count, uint8_t value)
{
	pj_size_t i;

	for (i = 0; i < count; ++i) {
		if (bytes[i] != value)
			return PJ_FALSE;
	}
	return PJ_TRUE;
}

static void scanner_error(pj_scanner *scanner)
{
	PJ_UNUSED_ARG(scanner);
	++scanner_error_count;
}

static int test_error_registration(void)
{
	const char *test = "error registration";
	char text[PJ_ERR_MSG_SIZE];
	pj_str_t result;

	result = pj_strerror(PJLIB_UTIL_EINXML, text, sizeof(text));
	/* PJ_BUILD_ERR appends the symbolic error name in the default profile. */
	CHECK_TRUE(test, str_starts_with(&result, "Invalid XML message"));
	CHECK_TRUE(test, text[result.slen] == '\0');
	return 0;
}

static int test_scanner(void)
{
	const char *test = "scanner";
	pj_cis_buf_t cis_buffer;
	pj_cis_buf_t exhaustion_buffer;
	pj_cis_t token;
	pj_cis_t slots[PJ_CIS_MAX_INDEX + 1];
	pj_scanner scanner;
	pj_scan_state state;
	pj_str_t value;
	unsigned i;
	char request[] = "  INVITE sip:user@example.com SIP/2.0\r\n";
	char newline[] = "\r\n next";
	char empty[] = "";
	struct {
		char data[32];
		uint32_t guard;
	} escaped = {"alice%20smith", GUARD_WORD};
	struct {
		char data[2];
		uint8_t guard[4];
	} malformed = {{'%', '\0'}, {GUARD_BYTE, GUARD_BYTE,
					       GUARD_BYTE, GUARD_BYTE}};

	pj_cis_buf_init(&cis_buffer);
	CHECK_STATUS(test, pj_cis_init(&cis_buffer, &token));
	pj_cis_add_alpha(&token);
	pj_cis_add_num(&token);
	pj_cis_add_str(&token, "-._~@");

	scanner_error_count = 0;
	pj_scan_init(&scanner, request, strlen(request), PJ_SCAN_AUTOSKIP_WS,
		     scanner_error);
	pj_scan_get(&scanner, &token, &value);
	CHECK_TRUE(test, str_equals(&value, "INVITE"));
	CHECK_TRUE(test, pj_scan_stricmp_alnum(&scanner, "sip", 3) == 0);
	pj_scan_save_state(&scanner, &state);
	pj_scan_advance_n(&scanner, 4, PJ_FALSE);
	CHECK_TRUE(test, pj_scan_get_char(&scanner) == 'u');
	pj_scan_restore_state(&scanner, &state);
	pj_scan_get_until_ch(&scanner, ' ', &value);
	CHECK_TRUE(test, str_equals(&value, "sip:user@example.com"));
	CHECK_TRUE(test, pj_scan_strcmp(&scanner, "SIP/2.0", 7) == 0);
	pj_scan_get_n(&scanner, 7, &value);
	CHECK_TRUE(test, str_equals(&value, "SIP/2.0"));
	pj_scan_get_newline(&scanner);
	CHECK_TRUE(test, pj_scan_is_eof(&scanner));
	CHECK_TRUE(test, scanner.line == 2);
	CHECK_TRUE(test, scanner_error_count == 0);
	pj_scan_fini(&scanner);

	pj_scan_init(&scanner, newline, strlen(newline),
		     PJ_SCAN_AUTOSKIP_WS | PJ_SCAN_AUTOSKIP_NEWLINE,
		     scanner_error);
	CHECK_TRUE(test, scanner.line == 2);
	CHECK_TRUE(test, pj_scan_get_col(&scanner) == 1);
	CHECK_TRUE(test, pj_scan_strcmp(&scanner, "next", 4) == 0);
	pj_scan_fini(&scanner);

	pj_scan_init(&scanner, empty, 0, 0, scanner_error);
	CHECK_TRUE(test, pj_scan_peek_n(&scanner, 1, &value) == -1);
	CHECK_TRUE(test, scanner_error_count == 1);
	CHECK_TRUE(test, scanner.curptr == scanner.begin);
	pj_scan_fini(&scanner);

	pj_scan_init(&scanner, escaped.data, strlen(escaped.data), 0,
		     scanner_error);
	pj_scan_get_unescape(&scanner, &token, &value);
	CHECK_TRUE(test, str_equals(&value, "alice smith"));
	CHECK_TRUE(test, pj_scan_is_eof(&scanner));
	CHECK_TRUE(test, escaped.guard == GUARD_WORD);
	pj_scan_fini(&scanner);

	pj_scan_init(&scanner, malformed.data, 1, 0, scanner_error);
	pj_scan_get_unescape(&scanner, &token, &value);
	CHECK_TRUE(test, bytes_are(malformed.guard, sizeof(malformed.guard),
				    GUARD_BYTE));
	CHECK_TRUE(test, pj_scan_is_eof(&scanner));
	pj_scan_fini(&scanner);

	pj_cis_buf_init(&exhaustion_buffer);
	for (i = 0; i < PJ_CIS_MAX_INDEX; ++i)
		CHECK_STATUS(test, pj_cis_init(&exhaustion_buffer, &slots[i]));
	CHECK_TRUE(test, pj_cis_init(&exhaustion_buffer,
				     &slots[PJ_CIS_MAX_INDEX]) == PJ_ETOOMANY);

	printk("[Phase 3] scanner boundaries/errors: PASSED\n");
	return 0;
}

static int test_string_utilities(pj_pool_t *pool)
{
	const char *test = "string utilities";
	pj_cis_buf_t cis_buffer;
	pj_cis_t unreserved;
	pj_str_t source = pj_str("alice smith@example.com");
	pj_str_t escaped_source = pj_str("alice%20smith%40example.com");
	pj_str_t malformed_source = pj_str("user%ZZ@example.com%");
	pj_str_t plain_source = pj_str("plain-user");
	pj_str_t result;
	struct {
		uint32_t before;
		char data[64];
		uint32_t after;
	} output = {GUARD_WORD, {0}, GUARD_WORD};
	struct {
		uint8_t before[4];
		char data[5];
		uint8_t after[4];
	} small;
	struct {
		uint32_t before;
		char data[64];
		uint32_t after;
	} copied = {GUARD_WORD, {0}, GUARD_WORD};

	pj_cis_buf_init(&cis_buffer);
	CHECK_STATUS(test, pj_cis_init(&cis_buffer, &unreserved));
	pj_cis_add_alpha(&unreserved);
	pj_cis_add_num(&unreserved);
	pj_cis_add_str(&unreserved, "-._~");

	result.ptr = output.data;
	CHECK_TRUE(test, pj_strncpy_escape(&result, &source,
					   sizeof(output.data), &unreserved) != NULL);
	CHECK_TRUE(test, str_equals(&result, "alice%20smith%40example.com"));
	CHECK_TRUE(test, output.before == GUARD_WORD &&
			 output.after == GUARD_WORD);

	memset(&small, GUARD_BYTE, sizeof(small));
	result.ptr = small.data;
	CHECK_TRUE(test, pj_strncpy_escape(&result, &source,
					   sizeof(small.data), &unreserved) == NULL);
	CHECK_TRUE(test, result.slen == -1);
	CHECK_TRUE(test, bytes_are(small.before, sizeof(small.before), GUARD_BYTE));
	CHECK_TRUE(test, bytes_are(small.after, sizeof(small.after), GUARD_BYTE));

	result = pj_str_unescape(pool, &escaped_source);
	CHECK_TRUE(test, str_equals(&result, "alice smith@example.com"));
	result = pj_str_unescape(pool, &malformed_source);
	CHECK_TRUE(test, str_equals(&result, "user%ZZ@example.com%"));
	result = pj_str_unescape(pool, &plain_source);
	CHECK_TRUE(test, result.ptr == plain_source.ptr &&
			 result.slen == plain_source.slen);

	result.ptr = copied.data;
	pj_strcpy_unescape(&result, &escaped_source);
	CHECK_TRUE(test, str_equals(&result, "alice smith@example.com"));
	CHECK_TRUE(test, copied.before == GUARD_WORD &&
			 copied.after == GUARD_WORD);

	printk("[Phase 3] string escape/unescape: PASSED\n");
	return 0;
}

static void digest_to_hex(const uint8_t digest[16], char output[33])
{
	static const char hex[] = "0123456789abcdef";
	unsigned i;

	for (i = 0; i < 16; ++i) {
		output[i * 2] = hex[digest[i] >> 4];
		output[i * 2 + 1] = hex[digest[i] & 0x0f];
	}
	output[32] = '\0';
}

static int test_md5(void)
{
	const char *test = "MD5";
	static const struct {
		const char *input;
		const char *expected;
	} vectors[] = {
		{"", "d41d8cd98f00b204e9800998ecf8427e"},
		{"a", "0cc175b9c0f1b6a831c399e269772661"},
		{"abc", "900150983cd24fb0d6963f7d28e17f72"},
		{"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
		{"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
	};
	unsigned vector_index;

	for (vector_index = 0; vector_index < PJ_ARRAY_SIZE(vectors);
	     ++vector_index) {
		struct {
			uint32_t before;
			pj_md5_context context;
			uint32_t after;
		} guarded_context = {GUARD_WORD, {{0}, {0}, {0}}, GUARD_WORD};
		struct {
			uint32_t before;
			uint8_t digest[16];
			uint32_t after;
		} guarded_digest = {GUARD_WORD, {0}, GUARD_WORD};
		char actual[33];
		pj_size_t input_len = strlen(vectors[vector_index].input);
		pj_size_t offset;

		pj_md5_init(&guarded_context.context);
		pj_md5_update(&guarded_context.context,
			      (const uint8_t *)vectors[vector_index].input, 0);
		for (offset = 0; offset < input_len; ++offset) {
			pj_md5_update(&guarded_context.context,
				      (const uint8_t *)vectors[vector_index].input +
				      offset, 1);
		}
		pj_md5_final(&guarded_context.context, guarded_digest.digest);
		digest_to_hex(guarded_digest.digest, actual);
		CHECK_TRUE(test, strcmp(actual, vectors[vector_index].expected) == 0);
		CHECK_TRUE(test, guarded_context.before == GUARD_WORD &&
				 guarded_context.after == GUARD_WORD);
		CHECK_TRUE(test, guarded_digest.before == GUARD_WORD &&
				 guarded_digest.after == GUARD_WORD);
	}

	printk("[Phase 3] MD5 RFC 1321 vectors: PASSED\n");
	return 0;
}

static int run_lifecycle(int iteration)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;
	int repeat;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);

	for (repeat = 1; repeat <= PHASE3_INIT_REPEATS; ++repeat) {
		status = pjlib_util_init();
		if (status != PJ_SUCCESS) {
			fail_status("pjlib_util_init", __LINE__, status);
			goto shutdown;
		}
	}
	if (test_error_registration() != 0)
		goto shutdown;

	pj_caching_pool_init(&caching_pool, NULL, 0);
	pool = pj_pool_create(&caching_pool.factory, "phase3", 4096, 4096, NULL);
	if (pool == NULL) {
		fail_value("pool setup", __LINE__, "pool != NULL");
		goto destroy_pool_factory;
	}

	if (test_scanner() != 0 ||
	    test_string_utilities(pool) != 0 ||
	    test_md5() != 0)
		goto release_pool;

	result = 0;

release_pool:
	pj_pool_release(pool);
	if (result == 0 &&
	    (caching_pool.used_count != 0 || caching_pool.capacity != 0)) {
		fail_value("pool cleanup", __LINE__,
			   "used_count == 0 && capacity == 0");
		result = -1;
	}
	if (result == 0)
		printk("[Phase 3] lifecycle %d: PASSED\n", iteration);
destroy_pool_factory:
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 3] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase3_util_run(void)
{
	int iteration;

	printk("[Phase 3] PJLIB-UTIL four-source validation ");
	printk("(%d repeated lifecycles)\n", PHASE3_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE3_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 3 RESULT: FAILED at lifecycle %d\n", iteration);
			return 1;
		}
	}

	printk("PHASE 3 RESULT: PASSED (%d/%d lifecycles, %d init calls)\n",
	       PHASE3_LIFECYCLES, PHASE3_LIFECYCLES,
	       PHASE3_LIFECYCLES * PHASE3_INIT_REPEATS);
	return 0;
}
