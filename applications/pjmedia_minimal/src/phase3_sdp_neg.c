#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/errno.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>

#include <stdio.h>
#include <string.h>

_Static_assert(PJMEDIA_MAX_SDP_MEDIA == 4, "unexpected media limit");
_Static_assert(PJMEDIA_MAX_SDP_FMT == 16, "unexpected format limit");
_Static_assert(PJMEDIA_SDP_MAX_PARSE_LEN == 4096,
	       "unexpected SDP input limit");

static const char local_capability[] =
	"v=0\r\n"
	"o=local 1 1 IN IP4 127.0.0.1\r\n"
	"s=phase3-local\r\n"
	"c=IN IP4 127.0.0.1\r\n"
	"t=0 0\r\n"
	"m=audio 4000 RTP/AVP 8 0 101\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-16\r\n"
	"a=sendrecv\r\n";

static const char remote_offer[] =
	"v=0\r\n"
	"o=remote 2 2 IN IP4 127.0.0.2\r\n"
	"s=phase3-remote\r\n"
	"c=IN IP4 127.0.0.2\r\n"
	"t=0 0\r\n"
	"m=audio 5000 RTP/AVP 0 8 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-15\r\n"
	"a=sendonly\r\n";

static const char remote_answer[] =
	"v=0\r\n"
	"o=remote 3 3 IN IP4 127.0.0.2\r\n"
	"s=phase3-answer\r\n"
	"c=IN IP4 127.0.0.2\r\n"
	"t=0 0\r\n"
	"m=audio 5000 RTP/AVP 0 101\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-15\r\n"
	"a=recvonly\r\n";

static const char inactive_offer[] =
	"v=0\r\n"
	"o=remote 4 4 IN IP4 127.0.0.2\r\n"
	"s=phase3-inactive\r\n"
	"c=IN IP4 127.0.0.2\r\n"
	"t=0 0\r\n"
	"m=audio 5000 RTP/AVP 0\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=inactive\r\n";

struct phase3_metrics {
	pj_size_t allocated_bytes;
	pj_size_t peak_bytes;
	unsigned allocated_blocks;
	unsigned peak_blocks;
	pj_size_t peak_pool_used;
};

static struct phase3_metrics metrics;
static pj_caching_pool *active_caching_pool;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 3] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_condition(const char *test, int line, const char *condition)
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

static pj_bool_t phase3_on_block_alloc(pj_pool_factory *factory,
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

static void phase3_on_block_free(pj_pool_factory *factory, pj_size_t size)
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

static pj_bool_t has_format(const pjmedia_sdp_media *media, const char *fmt)
{
	unsigned i;

	for (i = 0; i < media->desc.fmt_count; ++i) {
		if (pj_strcmp2(&media->desc.fmt[i], fmt) == 0)
			return PJ_TRUE;
	}
	return PJ_FALSE;
}

static int test_remote_offer_and_order(pj_pool_t *pool,
				       pj_bool_t prefer_remote,
				       const char *expected_codec)
{
	const char *test = prefer_remote ? "remote codec order" :
					 "local codec order";
	pjmedia_sdp_session *local;
	pjmedia_sdp_session *remote;
	pjmedia_sdp_neg *neg;
	const pjmedia_sdp_session *active;
	const pjmedia_sdp_session *pending;

	printk("[Phase 3] begin %s\n", test);

	CHECK_STATUS(test, parse_text(pool, local_capability, &local));
	CHECK_STATUS(test, parse_text(pool, remote_offer, &remote));
	CHECK_STATUS(test, pjmedia_sdp_neg_create_w_remote_offer(pool, local,
							       remote, &neg));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_WAIT_NEGO);
	CHECK_TRUE(test, pjmedia_sdp_neg_has_local_answer(neg));
	CHECK_STATUS(test, pjmedia_sdp_neg_get_neg_remote(neg, &pending));
	CHECK_TRUE(test, pending->media_count == 1);
	CHECK_STATUS(test, pjmedia_sdp_neg_set_prefer_remote_codec_order(
				neg, prefer_remote));
	CHECK_STATUS(test, pjmedia_sdp_neg_set_answer_multiple_codecs(neg,
				PJ_FALSE));
	CHECK_STATUS(test, pjmedia_sdp_neg_negotiate(pool, neg, PJ_FALSE));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);
	CHECK_TRUE(test, !pjmedia_sdp_neg_was_answer_remote(neg));
	CHECK_STATUS(test, pjmedia_sdp_neg_get_active_local(neg, &active));
	CHECK_TRUE(test, active->media_count == 1);
	CHECK_TRUE(test, pj_strcmp2(&active->media[0]->desc.fmt[0],
				   expected_codec) == 0);
	CHECK_TRUE(test, has_format(active->media[0], "101"));
	CHECK_TRUE(test, pjmedia_sdp_media_find_attr2(active->media[0],
						     "recvonly", NULL) != NULL);
	printk("[Phase 3] %s: PASSED\n", test);
	return 0;
}

static int test_local_offer_and_states(pj_pool_t *pool)
{
	const char *test = "local offer/states";
	pjmedia_sdp_session *local;
	pjmedia_sdp_session *answer;
	pjmedia_sdp_session *offer;
	pjmedia_sdp_neg *neg;
	const pjmedia_sdp_session *active;
	const pjmedia_sdp_session *pending;

	printk("[Phase 3] begin %s\n", test);

	CHECK_STATUS(test, parse_text(pool, local_capability, &local));
	CHECK_STATUS(test, parse_text(pool, remote_answer, &answer));
	CHECK_STATUS(test, parse_text(pool, remote_offer, &offer));
	CHECK_STATUS(test, pjmedia_sdp_neg_create_w_local_offer(pool, local,
							      &neg));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_LOCAL_OFFER);
	CHECK_STATUS(test, pjmedia_sdp_neg_get_neg_local(neg, &pending));
	CHECK_STATUS(test, pjmedia_sdp_neg_set_remote_answer(pool, neg, answer));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_WAIT_NEGO);
	CHECK_TRUE(test, !pjmedia_sdp_neg_has_local_answer(neg));
	CHECK_STATUS(test, pjmedia_sdp_neg_negotiate(pool, neg, PJ_FALSE));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);
	CHECK_TRUE(test, pjmedia_sdp_neg_was_answer_remote(neg));
	CHECK_STATUS(test, pjmedia_sdp_neg_get_active_local(neg, &active));
	CHECK_TRUE(test, has_format(active->media[0], "0"));
	CHECK_TRUE(test, has_format(active->media[0], "101"));
	CHECK_TRUE(test, pjmedia_sdp_media_find_attr2(active->media[0],
						     "sendonly", NULL) != NULL);

	CHECK_STATUS(test, pjmedia_sdp_neg_send_local_offer(pool, neg, &pending));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_LOCAL_OFFER);
	CHECK_STATUS(test, pjmedia_sdp_neg_cancel_offer(neg));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);

	CHECK_STATUS(test, pjmedia_sdp_neg_modify_local_offer(pool, neg, local));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_LOCAL_OFFER);
	CHECK_STATUS(test, pjmedia_sdp_neg_cancel_offer(neg));

	CHECK_STATUS(test, pjmedia_sdp_neg_set_remote_offer(pool, neg, offer));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_REMOTE_OFFER);
	CHECK_STATUS(test, pjmedia_sdp_neg_set_local_answer(pool, neg, local));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_WAIT_NEGO);
	CHECK_STATUS(test, pjmedia_sdp_neg_negotiate(pool, neg, PJ_FALSE));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);
	CHECK_TRUE(test, pjmedia_sdp_neg_state_str(PJMEDIA_SDP_NEG_STATE_DONE) !=
			 NULL);

	printk("[Phase 3] local offer/wait/done/modify/cancel/renegotiate: PASSED\n");
	return 0;
}

static int test_deferred_remote_answer(pj_pool_t *pool)
{
	const char *test = "deferred remote answer";
	pjmedia_sdp_session *local;
	pjmedia_sdp_session *remote;
	pjmedia_sdp_neg *neg;
	const pjmedia_sdp_session *active;

	CHECK_STATUS(test, parse_text(pool, local_capability, &local));
	CHECK_STATUS(test, parse_text(pool, inactive_offer, &remote));
	CHECK_STATUS(test, pjmedia_sdp_neg_create_w_remote_offer(pool, NULL,
							       remote, &neg));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_REMOTE_OFFER);
	CHECK_STATUS(test, pjmedia_sdp_neg_set_local_answer(pool, neg, local));
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_WAIT_NEGO);
	CHECK_STATUS(test, pjmedia_sdp_neg_negotiate(pool, neg, PJ_FALSE));
	CHECK_STATUS(test, pjmedia_sdp_neg_get_active_local(neg, &active));
	CHECK_TRUE(test, pjmedia_sdp_media_find_attr2(active->media[0],
						     "inactive", NULL) != NULL);
	CHECK_TRUE(test, pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);
	printk("[Phase 3] deferred remote offer/answer and inactive: PASSED\n");
	return 0;
}

static int test_rejections(pj_pool_t *pool)
{
	static const char two_media_local[] =
		"v=0\r\no=local 4 4 IN IP4 127.0.0.1\r\ns=two\r\n"
		"c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
		"m=audio 4000 RTP/AVP 0\r\n"
		"m=audio 4002 RTP/AVP 8\r\n";
	static const char rejected_offer[] =
		"v=0\r\no=remote 5 5 IN IP4 127.0.0.2\r\ns=reject\r\n"
		"c=IN IP4 127.0.0.2\r\nt=0 0\r\n"
		"m=audio 0 RTP/AVP 0\r\n"
		"m=audio 5002 RTP/AVP 8\r\n";
	static const char no_codec_offer[] =
		"v=0\r\no=remote 6 6 IN IP4 127.0.0.2\r\ns=none\r\n"
		"c=IN IP4 127.0.0.2\r\nt=0 0\r\n"
		"m=audio 5000 RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\n";
	static const char pcmu_local[] =
		"v=0\r\no=local 7 7 IN IP4 127.0.0.1\r\ns=pcmu\r\n"
		"c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
		"m=audio 4000 RTP/AVP 0\r\n";
	static const char bad_transport_answer[] =
		"v=0\r\no=remote 8 8 IN IP4 127.0.0.2\r\ns=bad-tp\r\n"
		"c=IN IP4 127.0.0.2\r\nt=0 0\r\n"
		"m=audio 5000 UDP 0\r\n";
	static const char missing_origin[] =
		"v=0\r\ns=malformed\r\nt=0 0\r\n"
		"m=audio 5000 RTP/AVP 0\r\n";
	pjmedia_sdp_session *local;
	pjmedia_sdp_session *remote;
	pjmedia_sdp_session *bad;
	pjmedia_sdp_neg *neg;
	const pjmedia_sdp_session *active;

	printk("[Phase 3] begin rejection cases\n");

	CHECK_STATUS("port zero", parse_text(pool, two_media_local, &local));
	CHECK_STATUS("port zero", parse_text(pool, rejected_offer, &remote));
	CHECK_STATUS("port zero", pjmedia_sdp_neg_create_w_remote_offer(
					pool, local, remote, &neg));
	CHECK_STATUS("port zero", pjmedia_sdp_neg_negotiate(pool, neg,
							    PJ_FALSE));
	CHECK_STATUS("port zero", pjmedia_sdp_neg_get_active_local(neg, &active));
	CHECK_TRUE("port zero", active->media_count == 2);
	CHECK_TRUE("port zero", active->media[0]->desc.port == 0);
	CHECK_TRUE("port zero", active->media[1]->desc.port != 0);

	CHECK_STATUS("no codec", parse_text(pool, pcmu_local, &local));
	CHECK_STATUS("no codec", parse_text(pool, no_codec_offer, &remote));
	CHECK_STATUS("no codec", pjmedia_sdp_neg_create_w_remote_offer(
					pool, local, remote, &neg));
	CHECK_EXPECTED("no codec", pjmedia_sdp_neg_negotiate(pool, neg,
							     PJ_FALSE),
		       PJMEDIA_SDPNEG_NOANSCODEC);
	CHECK_TRUE("no codec", pjmedia_sdp_neg_get_state(neg) ==
			 PJMEDIA_SDP_NEG_STATE_DONE);

	CHECK_STATUS("transport", parse_text(pool, pcmu_local, &local));
	CHECK_STATUS("transport", parse_text(pool, bad_transport_answer, &remote));
	CHECK_STATUS("transport", pjmedia_sdp_neg_create_w_local_offer(
					pool, local, &neg));
	CHECK_STATUS("transport", pjmedia_sdp_neg_set_remote_answer(
					pool, neg, remote));
	CHECK_EXPECTED("transport", pjmedia_sdp_neg_negotiate(pool, neg,
							      PJ_FALSE),
		       PJMEDIA_SDPNEG_EINVANSTP);

	CHECK_STATUS("malformed offer", parse_text(pool, missing_origin, &bad));
	CHECK_EXPECTED("malformed offer", pjmedia_sdp_validate(bad),
		       PJMEDIA_SDP_EINORIGIN);
	CHECK_EXPECTED("invalid remote offer",
		       pjmedia_sdp_neg_create_w_remote_offer(pool, local, bad, &neg),
		       PJMEDIA_SDP_EINORIGIN);
	CHECK_EXPECTED("malformed answer",
		       parse_text(pool,
			"v=0\no=- 1 1 IN IP4 127.0.0.1\ns=x\nt=0 0\n",
			&bad), PJMEDIA_SDP_EINSDP);

	printk("[Phase 3] port-zero/no-codec/transport/malformed rejection: PASSED\n");
	return 0;
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

static int test_configured_boundaries(pj_pool_t *pool)
{
	char exact[4096];
	char excessive[4096];
	char media[512];
	size_t length = 0;
	pjmedia_sdp_session *local;
	pjmedia_sdp_session *remote;
	pjmedia_sdp_neg *neg;
	unsigned i;

	printk("[Phase 3] begin configured boundaries\n");

	exact[0] = '\0';
	CHECK_TRUE("boundary setup", append_text(exact, sizeof(exact), &length,
		"v=0\r\no=local 9 9 IN IP4 127.0.0.1\r\ns=limit\r\n"
		"c=IN IP4 127.0.0.1\r\nt=0 0\r\n") == 0);
	for (i = 0; i < PJMEDIA_MAX_SDP_MEDIA; ++i) {
		int written = snprintf(media, sizeof(media),
			"m=audio %u RTP/AVP 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15\r\n",
			4000 + 2 * i);
		CHECK_TRUE("boundary setup", written > 0 &&
			   (size_t)written < sizeof(media));
		CHECK_TRUE("boundary setup", append_text(exact, sizeof(exact),
						       &length, media) == 0);
	}
	CHECK_STATUS("exact boundary", parse_text(pool, exact, &local));
	CHECK_STATUS("exact boundary", pjmedia_sdp_validate(local));
	CHECK_TRUE("exact boundary", local->media_count == PJMEDIA_MAX_SDP_MEDIA);
	for (i = 0; i < local->media_count; ++i)
		CHECK_TRUE("exact boundary", local->media[i]->desc.fmt_count ==
			   PJMEDIA_MAX_SDP_FMT);
	CHECK_STATUS("exact boundary", parse_text(pool, exact, &remote));
	CHECK_STATUS("exact boundary", pjmedia_sdp_neg_create_w_local_offer(
					pool, local, &neg));
	CHECK_STATUS("exact boundary", pjmedia_sdp_neg_set_remote_answer(
					pool, neg, remote));
	CHECK_STATUS("exact boundary", pjmedia_sdp_neg_negotiate(pool, neg,
								 PJ_FALSE));

	memcpy(excessive, exact, length + 1);
	CHECK_TRUE("excess boundary", append_text(excessive, sizeof(excessive),
						  &length,
						  "m=audio 4998 RTP/AVP 0\r\n") == 0);
	remote = NULL;
	CHECK_EXPECTED("excess boundary", parse_text(pool, excessive, &remote),
		       PJ_ETOOMANY);
	CHECK_TRUE("excess boundary", remote == NULL);

	printk("[Phase 3] reduced SDP exact-limit and overflow boundary: PASSED\n");
	return 0;
}

static int run_lifecycle(unsigned lifecycle)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool;
	pj_status_t status;
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

	pj_caching_pool_init(&caching_pool, NULL, 0);
	active_caching_pool = &caching_pool;
	caching_pool.factory.on_block_alloc = phase3_on_block_alloc;
	caching_pool.factory.on_block_free = phase3_on_block_free;
	pool = pj_pool_create(&caching_pool.factory, "phase3-neg", 65536, 32768,
			      NULL);
	if (pool == NULL) {
		status = PJ_ENOMEM;
		goto destroy_factory;
	}

	if (test_remote_offer_and_order(pool, PJ_TRUE, "0") != 0 ||
	    test_remote_offer_and_order(pool, PJ_FALSE, "8") != 0 ||
	    test_local_offer_and_states(pool) != 0 ||
	    test_deferred_remote_answer(pool) != 0 ||
	    test_rejections(pool) != 0 ||
	    test_configured_boundaries(pool) != 0)
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
		printk("[Phase 3] lifecycle %u pool teardown: PASSED\n", lifecycle);
	return result;
}

int phase3_sdp_neg_run(void)
{
	size_t stack_unused = 0;
	unsigned lifecycle;

	pj_bzero(&metrics, sizeof(metrics));
	printk("[Phase 3] SDP offer/answer negotiation validation\n");
	for (lifecycle = 1; lifecycle <= 3; ++lifecycle) {
		if (run_lifecycle(lifecycle) != 0)
			return 1;
	}

	if (k_thread_stack_space_get(k_current_get(), &stack_unused) != 0)
		return fail_condition("stack watermark", __LINE__,
				      "k_thread_stack_space_get success");
	printk("[Phase 3] resources: PJ blocks peak=%u/%u B, pool used peak=%u B\n",
	       metrics.peak_blocks, (unsigned)metrics.peak_bytes,
	       (unsigned)metrics.peak_pool_used);
	printk("[Phase 3] main stack: configured=%u B, used<=%u B, unused=%u B\n",
	       CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - stack_unused),
	       (unsigned)stack_unused);
	printk("PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)\n");
	return 0;
}
