#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/alaw_ulaw.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/errno.h>
#include <pjmedia/event.h>
#include <pjmedia/g711.h>
#include <pjsip.h>

#define PHASE7_LIFECYCLES 3
#define G711_SAMPLES 80
#define VECTOR_COUNT 8
#define BENCH_ITERATIONS 50000000

_Static_assert(PJ_HAS_FLOATING_POINT == 0,
	       "Phase 7 requires the fixed-point PJMEDIA profile");
_Static_assert(PJMEDIA_HAS_G711_CODEC == 1,
	       "Phase 7 requires G.711");
_Static_assert(PJMEDIA_HAS_ALAW_ULAW_TABLE == 0,
	       "Phase 7 uses algorithmic A-law/u-law conversion");
_Static_assert(PJMEDIA_HAS_VIDEO == 0, "Phase 7 is audio-only");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "Phase 7 excludes SRTP");

struct phase7_metrics {
	pj_size_t live_bytes;
	pj_size_t peak_bytes;
	unsigned live_blocks;
	unsigned peak_blocks;
	pj_size_t peak_pool_used;
	pj_uint64_t ulaw_msec;
	pj_uint64_t alaw_msec;
};

static struct phase7_metrics metrics;
static pj_caching_pool *active_caching_pool;
static unsigned endpoint_exit_count;
static unsigned event_callback_count;
static volatile unsigned benchmark_checksum;

static const pj_int16_t vector_pcm[VECTOR_COUNT] = {
	-30000, -1000, -1, 0, 1, 1000, 30000, 32767
};
static const pj_uint8_t vector_ulaw[VECTOR_COUNT] = {
	2, 78, 127, 255, 255, 206, 130, 128
};
static const pj_int16_t vector_ulaw_pcm[VECTOR_COUNT] = {
	-30076, -988, 0, 0, 0, 988, 30076, 32124
};
static const pj_uint8_t vector_alaw[VECTOR_COUNT] = {
	40, 122, 85, 213, 213, 250, 168, 170
};
static const pj_int16_t vector_alaw_pcm[VECTOR_COUNT] = {
	-30208, -1008, -8, 8, 8, 1008, 30208, 32256
};

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 7] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_condition(const char *test, int line, const char *condition)
{
	printk("[Phase 7] FAIL %s:%d condition=%s\n", test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                    \
		pj_status_t status_ = (expression);                               \
		if (status_ != PJ_SUCCESS)                                        \
			return fail_status((test_name), __LINE__, status_);          \
	} while (0)

#define CHECK_EXPECTED(test_name, expression, expected)                       \
	do {                                                                    \
		pj_status_t status_ = (expression);                               \
		if (status_ != (expected))                                       \
			return fail_status((test_name), __LINE__, status_);          \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                    \
		if (!(condition))                                                 \
			return fail_condition((test_name), __LINE__, #condition);    \
	} while (0)

static pj_bool_t phase7_on_block_alloc(pj_pool_factory *factory,
				       pj_size_t size)
{
	if (active_caching_pool == NULL ||
	    &active_caching_pool->factory != factory)
		return PJ_TRUE;

	metrics.live_bytes += size;
	metrics.live_blocks++;
	if (metrics.live_bytes > metrics.peak_bytes)
		metrics.peak_bytes = metrics.live_bytes;
	if (metrics.live_blocks > metrics.peak_blocks)
		metrics.peak_blocks = metrics.live_blocks;
	return PJ_TRUE;
}

static void phase7_on_block_free(pj_pool_factory *factory, pj_size_t size)
{
	if (active_caching_pool == NULL ||
	    &active_caching_pool->factory != factory)
		return;

	metrics.live_bytes -= size;
	metrics.live_blocks--;
}

static void endpoint_exit(pjmedia_endpt *endpt)
{
	PJ_UNUSED_ARG(endpt);
	endpoint_exit_count++;
}

static pj_status_t event_callback(pjmedia_event *event, void *user_data)
{
	unsigned *publisher = user_data;

	if (event->type != PJMEDIA_EVENT_FMT_CHANGED ||
	    event->epub != publisher)
		return PJ_EINVAL;
	event_callback_count++;
	return PJ_SUCCESS;
}

static int test_event_manager(pj_pool_t *pool)
{
	const char *test = "threadless event manager";
	pjmedia_event_mgr *manager = NULL;
	pjmedia_event event;
	unsigned publisher = 7;

	CHECK_STATUS(test, pjmedia_event_mgr_create(
		pool, PJMEDIA_EVENT_MGR_NO_THREAD, &manager));
	CHECK_TRUE(test, manager != NULL);
	CHECK_TRUE(test, pjmedia_event_mgr_instance() == manager);
	CHECK_STATUS(test, pjmedia_event_subscribe(
		manager, &event_callback, &publisher, &publisher));
	pjmedia_event_init(&event, PJMEDIA_EVENT_FMT_CHANGED, NULL, &publisher);
	CHECK_STATUS(test, pjmedia_event_publish(
		manager, &publisher, &event, PJMEDIA_EVENT_PUBLISH_DEFAULT));
	CHECK_TRUE(test, event_callback_count == 1);
	CHECK_STATUS(test, pjmedia_event_unsubscribe(
		manager, &event_callback, &publisher, &publisher));
	pjmedia_event_mgr_destroy(manager);
	CHECK_TRUE(test, pjmedia_event_mgr_instance() == NULL);
	printk("[Phase 7] threadless event publish/teardown: PASSED\n");
	return 0;
}

static int test_endpoint(pjmedia_endpt *media_endpt,
			 pjsip_endpoint *sip_endpt, pj_pool_t *pool)
{
	const char *test = "endpoint/ioqueue";
	pj_bool_t value = PJ_FALSE;
	pj_pool_t *child;
	pjmedia_sock_info sock_info;
	pjmedia_sdp_media *media = NULL;
	pj_str_t loopback = pj_str("127.0.0.1");
	unsigned i;
	pj_bool_t has_pcmu = PJ_FALSE;
	pj_bool_t has_pcma = PJ_FALSE;

	CHECK_TRUE(test, pjmedia_endpt_get_ioqueue(media_endpt) ==
			 pjsip_endpt_get_ioqueue(sip_endpt));
	CHECK_TRUE(test, pjmedia_endpt_get_thread_count(media_endpt) == 0);
	CHECK_STATUS(test, pjmedia_endpt_get_flag(
		media_endpt, PJMEDIA_ENDPT_HAS_TELEPHONE_EVENT_FLAG, &value));
	CHECK_TRUE(test, value == PJ_TRUE);
	value = PJ_FALSE;
	CHECK_STATUS(test, pjmedia_endpt_set_flag(
		media_endpt, PJMEDIA_ENDPT_HAS_TELEPHONE_EVENT_FLAG, &value));
	CHECK_STATUS(test, pjmedia_endpt_get_flag(
		media_endpt, PJMEDIA_ENDPT_HAS_TELEPHONE_EVENT_FLAG, &value));
	CHECK_TRUE(test, value == PJ_FALSE);
	CHECK_EXPECTED(test, pjmedia_endpt_set_flag(
		media_endpt, (pjmedia_endpt_flag)99, &value), PJ_EINVAL);

	child = pjmedia_endpt_create_pool(media_endpt, "phase7-child", 256, 256);
	CHECK_TRUE(test, child != NULL);
	pj_pool_release(child);

	pj_bzero(&sock_info, sizeof(sock_info));
	CHECK_STATUS(test, pj_sockaddr_in_init(
		&sock_info.rtp_addr_name.ipv4, &loopback, 4000));
	CHECK_STATUS(test, pj_sockaddr_in_init(
		&sock_info.rtcp_addr_name.ipv4, &loopback, 4001));
	CHECK_STATUS(test, pjmedia_endpt_create_audio_sdp(
		media_endpt, pool, &sock_info, NULL, &media));
	for (i = 0; i < media->desc.fmt_count; ++i) {
		if (pj_strcmp2(&media->desc.fmt[i], "0") == 0)
			has_pcmu = PJ_TRUE;
		if (pj_strcmp2(&media->desc.fmt[i], "8") == 0)
			has_pcma = PJ_TRUE;
	}
	CHECK_TRUE(test, has_pcmu && has_pcma);
	printk("[Phase 7] shared PJSIP ioqueue, zero media workers, endpoint SDP: PASSED\n");
	return 0;
}

static int test_codec(pjmedia_codec_mgr *manager,
		      const pjmedia_codec_info *info, pj_pool_t *pool,
		      const pj_uint8_t expected_code[VECTOR_COUNT],
		      const pj_int16_t expected_pcm[VECTOR_COUNT])
{
	const char *test = info->pt == PJMEDIA_RTP_PT_PCMU ? "PCMU" : "PCMA";
	pjmedia_codec_param param;
	pjmedia_codec_param changed;
	pjmedia_codec *codec = NULL;
	pjmedia_frame input;
	pjmedia_frame encoded_frame;
	pjmedia_frame decoded_frame;
	pjmedia_frame recovered_frame;
	pjmedia_frame parsed[2];
	pj_int16_t pcm[G711_SAMPLES];
	pj_int16_t decoded[G711_SAMPLES];
	pj_int16_t recovered[G711_SAMPLES];
	pj_int16_t silence[G711_SAMPLES];
	pj_uint8_t encoded[G711_SAMPLES];
	pj_uint8_t small_packet[G711_SAMPLES - 1];
	pj_timestamp timestamp;
	unsigned frame_count;
	unsigned i;

	for (i = 0; i < G711_SAMPLES; ++i)
		pcm[i] = vector_pcm[i % VECTOR_COUNT];
	pj_bzero(silence, sizeof(silence));
	pj_bzero(&timestamp, sizeof(timestamp));
	timestamp.u64 = G711_SAMPLES;

	CHECK_STATUS(test, pjmedia_codec_mgr_get_default_param(manager, info,
							     &param));
	CHECK_TRUE(test, param.info.clock_rate == 8000);
	CHECK_TRUE(test, param.info.channel_cnt == 1);
	CHECK_TRUE(test, param.info.pcm_bits_per_sample == 16);
	CHECK_STATUS(test, pjmedia_codec_mgr_alloc_codec(manager, info, &codec));
	CHECK_TRUE(test, codec != NULL);
	CHECK_STATUS(test, pjmedia_codec_init(codec, pool));
	param.setting.vad = 0;
	param.setting.plc = 0;
	CHECK_STATUS(test, pjmedia_codec_open(codec, &param));

	pj_bzero(&input, sizeof(input));
	pj_bzero(&encoded_frame, sizeof(encoded_frame));
	input.type = PJMEDIA_FRAME_TYPE_AUDIO;
	input.buf = pcm;
	input.size = sizeof(pcm);
	input.timestamp = timestamp;
	encoded_frame.buf = encoded;
	CHECK_EXPECTED(test, pjmedia_codec_encode(
		codec, &input, G711_SAMPLES - 1, &encoded_frame),
		PJMEDIA_CODEC_EFRMTOOSHORT);
	CHECK_STATUS(test, pjmedia_codec_encode(
		codec, &input, sizeof(encoded), &encoded_frame));
	CHECK_TRUE(test, encoded_frame.type == PJMEDIA_FRAME_TYPE_AUDIO);
	CHECK_TRUE(test, encoded_frame.size == G711_SAMPLES);
	for (i = 0; i < G711_SAMPLES; ++i)
		CHECK_TRUE(test, encoded[i] == expected_code[i % VECTOR_COUNT]);

	pj_bzero(&decoded_frame, sizeof(decoded_frame));
	decoded_frame.buf = decoded;
	CHECK_STATUS(test, pjmedia_codec_decode(
		codec, &encoded_frame, sizeof(decoded), &decoded_frame));
	CHECK_TRUE(test, decoded_frame.size == sizeof(decoded));
	for (i = 0; i < G711_SAMPLES; ++i) {
		if (decoded[i] != expected_pcm[i % VECTOR_COUNT]) {
			printk("[Phase 7] %s decode mismatch i=%u code=%u actual=%d expected=%d\n",
			       test, i, encoded[i], decoded[i],
			       expected_pcm[i % VECTOR_COUNT]);
			return fail_condition(test, __LINE__,
				"decoded sample matches fixed vector");
		}
	}

	frame_count = PJ_ARRAY_SIZE(parsed);
	CHECK_STATUS(test, pjmedia_codec_parse(codec, small_packet,
					       sizeof(small_packet), &timestamp,
					       &frame_count, parsed));
	CHECK_TRUE(test, frame_count == 0);

	changed = param;
	changed.setting.plc = 1;
	CHECK_STATUS(test, pjmedia_codec_modify(codec, &changed));
	CHECK_STATUS(test, pjmedia_codec_decode(
		codec, &encoded_frame, sizeof(decoded), &decoded_frame));
	pj_bzero(&recovered_frame, sizeof(recovered_frame));
	recovered_frame.buf = recovered;
	CHECK_STATUS(test, pjmedia_codec_recover(
		codec, sizeof(recovered), &recovered_frame));
	CHECK_TRUE(test, recovered_frame.size == sizeof(recovered));

	changed.setting.plc = 0;
	CHECK_STATUS(test, pjmedia_codec_modify(codec, &changed));
	CHECK_EXPECTED(test, pjmedia_codec_recover(
		codec, sizeof(recovered), &recovered_frame), PJ_EINVALIDOP);
	changed.info.pt = 96;
	CHECK_EXPECTED(test, pjmedia_codec_modify(codec, &changed),
		       PJMEDIA_EINVALIDPT);

	changed = param;
	changed.setting.vad = 1;
	changed.setting.plc = 0;
	CHECK_STATUS(test, pjmedia_codec_modify(codec, &changed));
	input.buf = silence;
	pj_bzero(&encoded_frame, sizeof(encoded_frame));
	encoded_frame.buf = encoded;
	CHECK_STATUS(test, pjmedia_codec_encode(
		codec, &input, sizeof(encoded), &encoded_frame));
	CHECK_TRUE(test, encoded_frame.type == PJMEDIA_FRAME_TYPE_NONE);
	changed.setting.vad = 0;
	CHECK_STATUS(test, pjmedia_codec_modify(codec, &changed));
	encoded_frame.buf = encoded;
	CHECK_STATUS(test, pjmedia_codec_encode(
		codec, &input, sizeof(encoded), &encoded_frame));
	CHECK_TRUE(test, encoded_frame.type == PJMEDIA_FRAME_TYPE_AUDIO);

	CHECK_STATUS(test, pjmedia_codec_close(codec));
	CHECK_STATUS(test, pjmedia_codec_mgr_dealloc_codec(manager, codec));
	printk("[Phase 7] %s vectors, short buffer, PLC and VAD controls: PASSED\n",
	       test);
	return 0;
}

static int test_codec_manager(pjmedia_endpt *media_endpt, pj_pool_t *pool)
{
	const char *test = "codec manager";
	pjmedia_codec_mgr *manager = pjmedia_endpt_get_codec_mgr(media_endpt);
	pjmedia_codec_info info[8];
	pjmedia_codec_info invalid;
	pjmedia_codec *codec = NULL;
	unsigned priorities[8];
	unsigned count = PJ_ARRAY_SIZE(info);
	pj_bool_t pcmu = PJ_FALSE;
	pj_bool_t pcma = PJ_FALSE;
	unsigned i;

	CHECK_TRUE(test, manager != NULL);
	CHECK_STATUS(test, pjmedia_codec_mgr_enum_codecs(
		manager, &count, info, priorities));
	CHECK_TRUE(test, count == 2);
	for (i = 0; i < count; ++i) {
		CHECK_TRUE(test, info[i].clock_rate == 8000);
		CHECK_TRUE(test, info[i].channel_cnt == 1);
		if (info[i].pt == PJMEDIA_RTP_PT_PCMU &&
		    pj_strcmp2(&info[i].encoding_name, "PCMU") == 0)
			pcmu = PJ_TRUE;
		else if (info[i].pt == PJMEDIA_RTP_PT_PCMA &&
			 pj_strcmp2(&info[i].encoding_name, "PCMA") == 0)
			pcma = PJ_TRUE;
		else
			return fail_condition(test, __LINE__,
				"only PCMU and PCMA are advertised");
	}
	CHECK_TRUE(test, pcmu && pcma);

	invalid = info[0];
	invalid.pt = 96;
	invalid.encoding_name = pj_str("invalid");
	CHECK_EXPECTED(test, pjmedia_codec_mgr_alloc_codec(
		manager, &invalid, &codec), PJMEDIA_CODEC_EUNSUP);
	CHECK_TRUE(test, codec == NULL);

	for (i = 0; i < count; ++i) {
		if (info[i].pt == PJMEDIA_RTP_PT_PCMU) {
			if (test_codec(manager, &info[i], pool, vector_ulaw,
				       vector_ulaw_pcm) != 0)
				return -1;
		} else {
			if (test_codec(manager, &info[i], pool, vector_alaw,
				       vector_alaw_pcm) != 0)
				return -1;
		}
	}
	printk("[Phase 7] codec manager advertises exactly PCMU and PCMA: PASSED\n");
	return 0;
}

static int benchmark_conversions(void)
{
	int64_t start;
	int64_t end;
	pj_uint64_t ulaw_msec;
	pj_uint64_t alaw_msec;
	unsigned checksum = 0;
	unsigned i;

	start = k_uptime_get();
	for (i = 0; i < BENCH_ITERATIONS; ++i)
		checksum += pjmedia_linear2ulaw(vector_pcm[i % VECTOR_COUNT]);
	end = k_uptime_get();
	ulaw_msec = end - start;
	metrics.ulaw_msec += ulaw_msec;

	start = k_uptime_get();
	for (i = 0; i < BENCH_ITERATIONS; ++i)
		checksum += pjmedia_linear2alaw(vector_pcm[i % VECTOR_COUNT]);
	end = k_uptime_get();
	alaw_msec = end - start;
	metrics.alaw_msec += alaw_msec;
	benchmark_checksum = checksum;
	printk("[Phase 7] QEMU conversion cost: u-law=%llu ms, A-law=%llu ms (%u calls each)\n",
	       (unsigned long long)ulaw_msec,
	       (unsigned long long)alaw_msec, BENCH_ITERATIONS);
	return 0;
}

static int run_lifecycle(unsigned lifecycle)
{
	pj_caching_pool caching_pool;
	pjsip_endpoint *sip_endpt = NULL;
	pjmedia_endpt *media_endpt = NULL;
	pj_pool_t *test_pool = NULL;
	pj_status_t status;
	int result = -1;

	endpoint_exit_count = 0;
	event_callback_count = 0;
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS)
		goto shutdown;
	pj_caching_pool_init(&caching_pool, NULL, 0);
	active_caching_pool = &caching_pool;
	caching_pool.factory.on_block_alloc = phase7_on_block_alloc;
	caching_pool.factory.on_block_free = phase7_on_block_free;

	status = pjsip_endpt_create(&caching_pool.factory, "phase7", &sip_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_factory;
	status = pjmedia_endpt_create2(&caching_pool.factory,
		pjsip_endpt_get_ioqueue(sip_endpt), 0, &media_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_sip;
	status = pjmedia_endpt_atexit(media_endpt, &endpoint_exit);
	if (status != PJ_SUCCESS)
		goto destroy_media;
	status = pjmedia_codec_g711_init(media_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_media;
	test_pool = pj_pool_create(&caching_pool.factory, "phase7-test",
				   32768, 16384, NULL);
	if (test_pool == NULL) {
		status = PJ_ENOMEM;
		goto deinit_g711;
	}

	if (test_endpoint(media_endpt, sip_endpt, test_pool) != 0 ||
	    test_event_manager(test_pool) != 0 ||
	    test_codec_manager(media_endpt, test_pool) != 0 ||
	    (lifecycle == 1 && benchmark_conversions() != 0))
		goto release_test_pool;
	if (pj_pool_get_used_size(test_pool) > metrics.peak_pool_used)
		metrics.peak_pool_used = pj_pool_get_used_size(test_pool);
	result = 0;

release_test_pool:
	pj_pool_release(test_pool);
deinit_g711:
	status = pjmedia_codec_g711_deinit();
	if (status != PJ_SUCCESS) {
		(void)fail_status("G.711 deinit", __LINE__, status);
		result = -1;
	}
destroy_media:
	if (media_endpt != NULL) {
		status = pjmedia_endpt_destroy2(media_endpt);
		if (status != PJ_SUCCESS) {
			(void)fail_status("media endpoint destroy", __LINE__, status);
			result = -1;
		}
	}
	if (endpoint_exit_count != 1) {
		(void)fail_condition("endpoint exit", __LINE__,
				     "exactly one endpoint exit callback");
		result = -1;
	}
destroy_sip:
	if (sip_endpt != NULL)
		pjsip_endpt_destroy(sip_endpt);
destroy_factory:
	if (caching_pool.used_count != 0 || metrics.live_bytes != 0 ||
	    metrics.live_blocks != 0) {
		(void)fail_condition("pool teardown", __LINE__,
				     "zero checked-out pools and tracked blocks");
		result = -1;
	}
	active_caching_pool = NULL;
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	if (status != PJ_SUCCESS)
		return fail_status("lifecycle", __LINE__, status);
	if (result == 0)
		printk("[Phase 7] lifecycle %u endpoint/codec teardown: PASSED\n",
		       lifecycle);
	return result;
}

int phase7_g711_run(void)
{
	size_t stack_unused = 0;
	unsigned lifecycle;

	pj_bzero(&metrics, sizeof(metrics));
	printk("[Phase 7] PJMEDIA endpoint and G.711 validation (%d lifecycles)\n",
	       PHASE7_LIFECYCLES);
	for (lifecycle = 1; lifecycle <= PHASE7_LIFECYCLES; ++lifecycle) {
		if (run_lifecycle(lifecycle) != 0) {
			printk("PHASE 7 RESULT: FAILED at lifecycle %u\n", lifecycle);
			return 1;
		}
	}
	if (k_thread_stack_space_get(k_current_get(), &stack_unused) != 0)
		return fail_condition("stack watermark", __LINE__,
				      "k_thread_stack_space_get success");
	printk("[Phase 7] resources: PJ blocks peak=%u/%u B, test pool used peak=%u B\n",
	       metrics.peak_blocks, (unsigned)metrics.peak_bytes,
	       (unsigned)metrics.peak_pool_used);
	printk("[Phase 7] main stack: configured=%u B, used<=%u B, unused=%u B\n",
	       (unsigned)CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - stack_unused),
	       (unsigned)stack_unused);
	printk("PHASE 7 RESULT: PASSED (3 endpoint/G.711 lifecycles; PCMU+PCMA)\n");
	return 0;
}
