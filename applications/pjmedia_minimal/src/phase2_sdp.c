#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/errno.h>
#include <pjmedia/sdp.h>

#include <stdio.h>
#include <string.h>

_Static_assert(PJMEDIA_MAX_SDP_MEDIA == 4, "unexpected media limit");
_Static_assert(PJMEDIA_MAX_SDP_FMT == 16, "unexpected format limit");
_Static_assert(PJMEDIA_MAX_SDP_ATTR == 36, "unexpected attribute limit");
_Static_assert(PJMEDIA_SDP_MAX_PARSE_LEN == 4096,
	       "unexpected SDP input limit");

static const char full_sdp[] =
	"v=0\r\n"
	"o=alice 100 101 IN IP4 127.0.0.1\r\n"
	"s=Zephyr Phase 2\r\n"
	"c=IN IP4 127.0.0.1\r\n"
	"b=AS:80\r\n"
	"t=0 0\r\n"
	"a=tool:pjmedia-phase2\r\n"
	"m=audio 4000 RTP/AVP 0 8 101\r\n"
	"c=IN IP4 127.0.0.2\r\n"
	"b=TIAS:64000\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=sendrecv\r\n";

static const char minimal_sdp[] =
	"v=0\r\n"
	"o=- 1 1 IN IP4 127.0.0.1\r\n"
	"s=minimal\r\n"
	"t=0 0\r\n";

struct phase2_metrics {
	pj_size_t allocated_bytes;
	pj_size_t peak_bytes;
	unsigned allocated_blocks;
	unsigned peak_blocks;
	pj_size_t peak_pool_used;
};

static struct phase2_metrics metrics;
static pj_caching_pool *active_caching_pool;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 2] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_condition(const char *test, int line, const char *condition)
{
	printk("[Phase 2] FAIL %s:%d condition=%s\n",
	       test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                     \
		pj_status_t check_status_ = (expression);                           \
		if (check_status_ != PJ_SUCCESS)                                    \
			return fail_status((test_name), __LINE__, check_status_);      \
	} while (0)

#define CHECK_EXPECTED(test_name, expression, expected)                       \
	do {                                                                     \
		pj_status_t check_status_ = (expression);                           \
		if (check_status_ != (expected))                                   \
			return fail_status((test_name), __LINE__, check_status_);      \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                     \
		if (!(condition))                                                  \
			return fail_condition((test_name), __LINE__, #condition);     \
	} while (0)

static pj_bool_t phase2_on_block_alloc(pj_pool_factory *factory,
				       pj_size_t size)
{
	if (active_caching_pool == NULL ||
	    &active_caching_pool->factory != factory)
		return PJ_TRUE;

	metrics.allocated_bytes += size;
	metrics.allocated_blocks++;
	if (metrics.allocated_bytes > metrics.peak_bytes)
		metrics.peak_bytes = metrics.allocated_bytes;
	if (metrics.allocated_blocks > metrics.peak_blocks)
		metrics.peak_blocks = metrics.allocated_blocks;
	return PJ_TRUE;
}

static void phase2_on_block_free(pj_pool_factory *factory, pj_size_t size)
{
	if (active_caching_pool == NULL ||
	    &active_caching_pool->factory != factory)
		return;

	metrics.allocated_bytes -= size;
	metrics.allocated_blocks--;
}

static pj_status_t parse_text(pj_pool_t *pool, const char *text,
			      pjmedia_sdp_session **session)
{
	pj_size_t length = pj_ansi_strlen(text);
	char *copy = pj_pool_alloc(pool, length + 1);

	if (copy == NULL)
		return PJ_ENOMEM;
	pj_memcpy(copy, text, length + 1);
	return pjmedia_sdp_parse(pool, copy, length, session);
}

static int append_text(char *buffer, size_t capacity, size_t *length,
		       const char *text)
{
	size_t addition = strlen(text);

	if (*length + addition >= capacity)
		return -1;
	memcpy(buffer + *length, text, addition + 1);
	*length += addition;
	return 0;
}

static int test_valid_parse_print_clone(pj_pool_t *pool)
{
	const char *test = "valid/print/clone";
	pjmedia_sdp_session *session;
	pjmedia_sdp_session *reparsed;
	pjmedia_sdp_session *clone;
	pjmedia_sdp_media *media;
	pjmedia_sdp_attr *attribute;
	pjmedia_sdp_rtpmap rtpmap;
	pjmedia_sdp_fmtp fmtp;
	pj_str_t payload;
	char printed[2048];
	char *reparse_copy;
	int printed_length;

	CHECK_STATUS(test, parse_text(pool, minimal_sdp, &session));
	CHECK_STATUS(test, pjmedia_sdp_validate(session));
	CHECK_TRUE(test, session->media_count == 0);

	CHECK_STATUS(test, parse_text(pool, full_sdp, &session));
	CHECK_STATUS(test, pjmedia_sdp_validate(session));
	CHECK_TRUE(test, pj_strcmp2(&session->origin.net_type, "IN") == 0);
	CHECK_TRUE(test, pj_strcmp2(&session->origin.addr_type, "IP4") == 0);
	CHECK_TRUE(test, pj_strcmp2(&session->origin.addr, "127.0.0.1") == 0);
	CHECK_TRUE(test, session->conn != NULL);
	CHECK_TRUE(test, pj_strcmp2(&session->conn->addr_type, "IP4") == 0);
	CHECK_TRUE(test, session->bandw_count == 1);
	CHECK_TRUE(test, session->attr_count == 1);
	CHECK_TRUE(test, session->media_count == 1);

	media = session->media[0];
	CHECK_TRUE(test, media->conn != NULL);
	CHECK_TRUE(test, media->bandw_count == 1);
	CHECK_TRUE(test, media->desc.fmt_count == 3);
	CHECK_TRUE(test, pj_strcmp2(&media->desc.fmt[0], "0") == 0);
	CHECK_TRUE(test, pj_strcmp2(&media->desc.fmt[1], "8") == 0);
	CHECK_TRUE(test, pj_strcmp2(&media->desc.fmt[2], "101") == 0);

	payload = pj_str("0");
	attribute = pjmedia_sdp_media_find_attr2(media, "rtpmap", &payload);
	CHECK_TRUE(test, attribute != NULL);
	CHECK_STATUS(test, pjmedia_sdp_attr_get_rtpmap(attribute, &rtpmap));
	CHECK_TRUE(test, pj_strcmp2(&rtpmap.enc_name, "PCMU") == 0);
	CHECK_TRUE(test, rtpmap.clock_rate == 8000);
	payload = pj_str("8");
	attribute = pjmedia_sdp_media_find_attr2(media, "rtpmap", &payload);
	CHECK_TRUE(test, attribute != NULL);
	CHECK_STATUS(test, pjmedia_sdp_attr_get_rtpmap(attribute, &rtpmap));
	CHECK_TRUE(test, pj_strcmp2(&rtpmap.enc_name, "PCMA") == 0);
	CHECK_TRUE(test, rtpmap.clock_rate == 8000);
	payload = pj_str("101");
	attribute = pjmedia_sdp_media_find_attr2(media, "rtpmap", &payload);
	CHECK_TRUE(test, attribute != NULL);
	CHECK_STATUS(test, pjmedia_sdp_attr_get_rtpmap(attribute, &rtpmap));
	CHECK_TRUE(test, pj_strcmp2(&rtpmap.enc_name, "telephone-event") == 0);
	CHECK_TRUE(test, rtpmap.clock_rate == 8000);
	attribute = pjmedia_sdp_media_find_attr2(media, "fmtp", &payload);
	CHECK_TRUE(test, attribute != NULL);
	CHECK_STATUS(test, pjmedia_sdp_attr_get_fmtp(attribute, &fmtp));
	CHECK_TRUE(test, pj_strcmp2(&fmtp.fmt, "101") == 0);
	CHECK_TRUE(test, pj_strcmp2(&fmtp.fmt_param, "0-16") == 0);

	printed_length = pjmedia_sdp_print(session, printed, sizeof(printed));
	CHECK_TRUE(test, printed_length > 0);
	CHECK_TRUE(test, (size_t)printed_length < sizeof(printed));
	printed[printed_length] = '\0';
	reparse_copy = pj_pool_alloc(pool, (pj_size_t)printed_length + 1);
	CHECK_TRUE(test, reparse_copy != NULL);
	pj_memcpy(reparse_copy, printed, (pj_size_t)printed_length + 1);
	CHECK_STATUS(test, pjmedia_sdp_parse(pool, reparse_copy,
					   (pj_size_t)printed_length, &reparsed));
	CHECK_STATUS(test, pjmedia_sdp_validate(reparsed));
	CHECK_STATUS(test, pjmedia_sdp_session_cmp(session, reparsed, 0));
	CHECK_TRUE(test, pjmedia_sdp_print(session, printed, 32) == -1);

	clone = pjmedia_sdp_session_clone(pool, session);
	CHECK_TRUE(test, clone != NULL && clone != session);
	CHECK_TRUE(test, clone->conn != session->conn);
	CHECK_TRUE(test, clone->bandw[0] != session->bandw[0]);
	CHECK_TRUE(test, clone->attr[0] != session->attr[0]);
	CHECK_TRUE(test, clone->media[0] != session->media[0]);
	CHECK_TRUE(test, clone->media[0]->conn != session->media[0]->conn);
	CHECK_TRUE(test, clone->media[0]->bandw[0] !=
			 session->media[0]->bandw[0]);
	CHECK_TRUE(test, clone->media[0]->attr[0] != session->media[0]->attr[0]);
	CHECK_STATUS(test, pjmedia_sdp_session_cmp(session, clone, 0));
	clone->origin.version++;
	CHECK_TRUE(test, pjmedia_sdp_session_cmp(session, clone, 0) != PJ_SUCCESS);

	printk("[Phase 2] valid parse/print/reparse/deep-clone/compare: PASSED\n");
	return 0;
}

static int test_directions(pj_pool_t *pool)
{
	static const char *const directions[] = {
		"sendrecv", "sendonly", "recvonly", "inactive"
	};
	char text[512];
	unsigned i;

	for (i = 0; i < PJ_ARRAY_SIZE(directions); ++i) {
		pjmedia_sdp_session *session;
		int length = snprintf(text, sizeof(text),
			"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n"
			"s=direction\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n"
			"m=audio 4000 RTP/AVP 0\r\na=%s\r\n",
			directions[i]);

		CHECK_TRUE("directions", length > 0 && (size_t)length < sizeof(text));
		CHECK_STATUS("directions", parse_text(pool, text, &session));
		CHECK_STATUS("directions", pjmedia_sdp_validate(session));
		CHECK_TRUE("directions",
			   pjmedia_sdp_media_find_attr2(session->media[0],
							 directions[i], NULL) != NULL);
	}

	printk("[Phase 2] sendrecv/sendonly/recvonly/inactive: PASSED\n");
	return 0;
}

static int expect_parse_failure(pj_pool_t *pool, const char *name,
				const char *text, pj_status_t expected)
{
	pjmedia_sdp_session *session = NULL;
	pj_status_t status = parse_text(pool, text, &session);

	if (status != expected || session != NULL)
		return fail_status(name, __LINE__, status);
	return 0;
}

static int test_invalid_inputs(pj_pool_t *pool)
{
	static char oversized[PJMEDIA_SDP_MAX_PARSE_LEN + 2];
	char excessive[4096];
	char line[96];
	size_t length;
	pjmedia_sdp_session *session;
	pj_status_t status;
	unsigned i;

	CHECK_STATUS("missing mandatory parse",
		     parse_text(pool, "v=0\r\ns=missing-origin\r\nt=0 0\r\n",
				&session));
	CHECK_EXPECTED("missing mandatory validate",
		       pjmedia_sdp_validate(session), PJMEDIA_SDP_EINORIGIN);

	CHECK_STATUS("invalid payload parse",
		     parse_text(pool,
			"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=bad-pt\r\n"
			"c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
			"m=audio 4000 RTP/AVP 128\r\n", &session));
	CHECK_EXPECTED("invalid payload validate",
		       pjmedia_sdp_validate(session), PJMEDIA_SDP_EINPT);

	CHECK_TRUE("bad LF endings",
		   expect_parse_failure(pool, "bad LF endings",
			"v=0\no=- 1 1 IN IP4 127.0.0.1\ns=x\nt=0 0\n",
			PJMEDIA_SDP_EINSDP) == 0);
	CHECK_TRUE("truncated input",
		   expect_parse_failure(pool, "truncated input",
			"v=0\r\no=- 1 1 IN IP4", PJMEDIA_SDP_EINORIGIN) == 0);

	memset(oversized, 'x', sizeof(oversized));
	oversized[sizeof(oversized) - 1] = '\0';
	status = pjmedia_sdp_parse(pool, oversized, sizeof(oversized) - 1,
				   &session);
	CHECK_TRUE("overlong input", status == PJ_ETOOBIG && session == NULL);

	length = 0;
	excessive[0] = '\0';
	CHECK_TRUE("excess media setup",
		   append_text(excessive, sizeof(excessive), &length,
			"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=media-limit\r\n"
			"c=IN IP4 127.0.0.1\r\nt=0 0\r\n") == 0);
	for (i = 0; i <= PJMEDIA_MAX_SDP_MEDIA; ++i)
		CHECK_TRUE("excess media setup",
			   append_text(excessive, sizeof(excessive), &length,
				       "m=audio 4000 RTP/AVP 0\r\n") == 0);
	CHECK_TRUE("excess media",
		   expect_parse_failure(pool, "excess media", excessive,
					PJ_ETOOMANY) == 0);

	length = 0;
	excessive[0] = '\0';
	CHECK_TRUE("excess format setup",
		   append_text(excessive, sizeof(excessive), &length,
			"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=format-limit\r\n"
			"c=IN IP4 127.0.0.1\r\nt=0 0\r\nm=audio 4000 RTP/AVP") == 0);
	for (i = 0; i <= PJMEDIA_MAX_SDP_FMT; ++i) {
		(void)snprintf(line, sizeof(line), " %u", i);
		CHECK_TRUE("excess format setup",
			   append_text(excessive, sizeof(excessive), &length, line) == 0);
	}
	CHECK_TRUE("excess format setup",
		   append_text(excessive, sizeof(excessive), &length, "\r\n") == 0);
	CHECK_TRUE("excess format",
		   expect_parse_failure(pool, "excess format", excessive,
					PJ_ETOOMANY) == 0);

	length = 0;
	excessive[0] = '\0';
	CHECK_TRUE("excess attribute setup",
		   append_text(excessive, sizeof(excessive), &length,
			"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=attr-limit\r\n"
			"c=IN IP4 127.0.0.1\r\nt=0 0\r\n") == 0);
	for (i = 0; i <= PJMEDIA_MAX_SDP_ATTR; ++i) {
		(void)snprintf(line, sizeof(line), "a=x-%u:value\r\n", i);
		CHECK_TRUE("excess attribute setup",
			   append_text(excessive, sizeof(excessive), &length, line) == 0);
	}
	CHECK_TRUE("excess attribute",
		   expect_parse_failure(pool, "excess attribute", excessive,
					PJ_ETOOMANY) == 0);

	printk("[Phase 2] malformed/truncated/overlong/configured limits: PASSED\n");
	return 0;
}

static int run_lifecycle(unsigned lifecycle)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;
	char error_text[PJ_ERR_MSG_SIZE];
	pj_str_t error;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS)
		goto shutdown;
	status = pj_register_strerror(PJMEDIA_ERRNO_START, PJ_ERRNO_SPACE_SIZE,
				      &pjmedia_strerror);
	if (status != PJ_SUCCESS)
		goto shutdown;
	error = pj_strerror(PJMEDIA_SDP_EINPT, error_text, sizeof(error_text));
	if (error.slen == 0) {
		status = PJ_EUNKNOWN;
		goto shutdown;
	}

	pj_caching_pool_init(&caching_pool, NULL, 0);
	active_caching_pool = &caching_pool;
	caching_pool.factory.on_block_alloc = phase2_on_block_alloc;
	caching_pool.factory.on_block_free = phase2_on_block_free;
	pool = pj_pool_create(&caching_pool.factory, "phase2-sdp", 32768, 16384,
			      NULL);
	if (pool == NULL) {
		status = PJ_ENOMEM;
		goto destroy_factory;
	}

	if (test_valid_parse_print_clone(pool) != 0 ||
	    test_directions(pool) != 0 || test_invalid_inputs(pool) != 0)
		goto release_pool;
	if (pj_pool_get_used_size(pool) > metrics.peak_pool_used)
		metrics.peak_pool_used = pj_pool_get_used_size(pool);
	result = 0;

release_pool:
	pj_pool_release(pool);
	if (caching_pool.used_count != 0 || metrics.allocated_bytes != 0 ||
	    metrics.allocated_blocks != 0) {
		(void)fail_condition("pool teardown", __LINE__,
				     "zero checked-out pools and blocks");
		result = -1;
	}
destroy_factory:
	active_caching_pool = NULL;
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	if (status != PJ_SUCCESS)
		return fail_status("lifecycle", __LINE__, status);
	if (result == 0)
		printk("[Phase 2] lifecycle %u pool teardown: PASSED\n", lifecycle);
	return result;
}

int phase2_sdp_run(void)
{
	size_t stack_unused = 0;
	unsigned lifecycle;

	pj_bzero(&metrics, sizeof(metrics));
	printk("[Phase 2] SDP representation/parser/printer validation\n");
	for (lifecycle = 1; lifecycle <= 3; ++lifecycle) {
		if (run_lifecycle(lifecycle) != 0)
			return 1;
	}

	if (k_thread_stack_space_get(k_current_get(), &stack_unused) != 0)
		return fail_condition("stack watermark", __LINE__,
				      "k_thread_stack_space_get success");
	printk("[Phase 2] resources: PJ blocks peak=%u/%u B, pool used peak=%u B\n",
	       metrics.peak_blocks, (unsigned)metrics.peak_bytes,
	       (unsigned)metrics.peak_pool_used);
	printk("[Phase 2] main stack: configured=%u B, used<=%u B, unused=%u B\n",
	       CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - stack_unused),
	       (unsigned)stack_unused);
	printk("PHASE 2 RESULT: PASSED (3 complete SDP lifecycles)\n");
	return 0;
}
