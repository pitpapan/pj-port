#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/errno.h>
#include <pjmedia/jbuf.h>
#include <pjmedia/rtcp.h>
#include <pjmedia/rtcp_fb.h>
#include <pjmedia/rtp.h>

#define PHASE8_LIFECYCLES 3
#define RTP_PT 0
#define RTP_SSRC 0x12345678u
#define FRAME_SIZE 4
#define BENCH_ITERATIONS 200000

_Static_assert(PJ_HAS_FLOATING_POINT == 0,
	       "Phase 8 requires the fixed-point PJMEDIA profile");
_Static_assert(PJMEDIA_HAS_VIDEO == 0, "Phase 8 excludes video");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "Phase 8 excludes SRTP");
_Static_assert(PJMEDIA_HAS_RTCP_XR == 0, "Phase 8 excludes RTCP XR");

struct phase8_metrics {
	pj_size_t live_bytes;
	pj_size_t peak_bytes;
	unsigned live_blocks;
	unsigned peak_blocks;
	pj_size_t peak_pool_used;
	unsigned max_jbuf_frames;
};

static struct phase8_metrics metrics;
static pj_caching_pool *active_caching_pool;
static volatile unsigned benchmark_checksum;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 8] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_condition(const char *test, int line, const char *condition)
{
	printk("[Phase 8] FAIL %s:%d condition=%s\n", test, line, condition);
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

static pj_bool_t phase8_on_block_alloc(pj_pool_factory *factory,
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

static void phase8_on_block_free(pj_pool_factory *factory, pj_size_t size)
{
	if (active_caching_pool == NULL ||
	    &active_caching_pool->factory != factory)
		return;
	metrics.live_bytes -= size;
	metrics.live_blocks--;
}

static void set_rtp_header(pjmedia_rtp_hdr *hdr, unsigned pt,
			   pj_uint16_t seq, pj_uint32_t ts, pj_uint32_t ssrc)
{
	pj_bzero(hdr, sizeof(*hdr));
	hdr->v = 2;
	hdr->pt = (pj_uint8_t)pt;
	hdr->seq = pj_htons(seq);
	hdr->ts = pj_htonl(ts);
	hdr->ssrc = pj_htonl(ssrc);
}

static int test_rtp(void)
{
	const char *test = "RTP packet/session";
	pjmedia_rtp_session tx;
	pjmedia_rtp_session rx;
	pjmedia_rtp_session_setting setting;
	pjmedia_rtp_status seq_status;
	pjmedia_rtp_hdr input;
	const pjmedia_rtp_hdr *encoded;
	const pjmedia_rtp_hdr *decoded;
	pjmedia_rtp_dec_hdr dec_hdr;
	const void *payload;
	unsigned payload_len;
	int header_len;
	pj_uint32_t packet_words[8];
	pjmedia_rtp_hdr *packet = (pjmedia_rtp_hdr *)packet_words;
	pjmedia_rtp_ext_hdr *extension;

	pj_bzero(&setting, sizeof(setting));
	setting.flags = 1 | 2 | 4 | 8;
	setting.default_pt = RTP_PT;
	setting.sender_ssrc = RTP_SSRC;
	setting.seq = 65534;
	setting.ts = 0xfffffff0u;
	CHECK_STATUS(test, pjmedia_rtp_session_init2(&tx, setting));
	CHECK_STATUS(test, pjmedia_rtp_encode_rtp(&tx, -1, 1, 160, 32,
						 (const void **)&encoded,
						 &header_len));
	CHECK_TRUE(test, header_len == 12);
	CHECK_TRUE(test, encoded->v == 2 && encoded->pt == RTP_PT && encoded->m);
	CHECK_TRUE(test, pj_ntohs(encoded->seq) == 65535);
	CHECK_TRUE(test, pj_ntohl(encoded->ts) == 16);
	CHECK_TRUE(test, pj_ntohl(encoded->ssrc) == RTP_SSRC);
	CHECK_STATUS(test, pjmedia_rtp_encode_rtp(&tx, -1, 0, 160, 160,
						 (const void **)&encoded,
						 &header_len));
	CHECK_TRUE(test, pj_ntohs(encoded->seq) == 0);
	CHECK_TRUE(test, pj_ntohl(encoded->ts) == 176);

	pj_bzero(packet_words, sizeof(packet_words));
	set_rtp_header(packet, RTP_PT, 7, 320, RTP_SSRC);
	((pj_uint8_t *)packet_words)[12] = 0x11;
	((pj_uint8_t *)packet_words)[13] = 0x22;
	CHECK_STATUS(test, pjmedia_rtp_decode_rtp2(&rx, packet_words, 14,
						 &decoded, &dec_hdr, &payload,
						 &payload_len));
	CHECK_TRUE(test, decoded == packet && payload_len == 2);
	CHECK_TRUE(test, payload == &((pj_uint8_t *)packet_words)[12]);
	CHECK_TRUE(test, dec_hdr.ext_hdr == NULL && dec_hdr.ext_len == 0);

	packet->v = 1;
	CHECK_EXPECTED(test, pjmedia_rtp_decode_rtp(&rx, packet_words, 14,
						     &decoded, &payload,
						     &payload_len),
		       PJMEDIA_RTP_EINVER);
	set_rtp_header(packet, RTP_PT, 7, 320, RTP_SSRC);
	CHECK_EXPECTED(test, pjmedia_rtp_decode_rtp(&rx, packet_words, 11,
						     &decoded, &payload,
						     &payload_len),
		       PJMEDIA_RTP_EINLEN);
	packet->cc = 15;
	CHECK_EXPECTED(test, pjmedia_rtp_decode_rtp(&rx, packet_words, 12,
						     &decoded, &payload,
						     &payload_len),
		       PJMEDIA_RTP_EINLEN);
	set_rtp_header(packet, RTP_PT, 7, 320, RTP_SSRC);
	packet->x = 1;
	CHECK_EXPECTED(test, pjmedia_rtp_decode_rtp2(&rx, packet_words, 12,
						      &decoded, &dec_hdr, &payload,
						      &payload_len),
		       PJMEDIA_RTP_EINLEN);
	extension = (pjmedia_rtp_ext_hdr *)&((pj_uint8_t *)packet_words)[12];
	extension->profile_data = pj_htons(0x1000);
	extension->length = pj_htons(0xffff);
	CHECK_EXPECTED(test, pjmedia_rtp_decode_rtp2(&rx, packet_words, 16,
						      &decoded, &dec_hdr, &payload,
						      &payload_len),
		       PJMEDIA_RTP_EINLEN);

	CHECK_STATUS(test, pjmedia_rtp_session_init(&rx, RTP_PT, 0xaaaaaaaau));
	set_rtp_header(&input, RTP_PT, 65534, 0, RTP_SSRC);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.probation && seq_status.diff == 1);
	input.seq = pj_htons(65535);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, !seq_status.status.flag.bad && seq_status.diff == 1);
	input.seq = pj_htons(0);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, rx.seq_ctrl.cycles == 65536 && seq_status.diff == 1);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.dup);
	input.seq = pj_htons(2);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.diff == 2);
	input.seq = pj_htons(1);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.outorder);
	input.pt = 8;
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.bad && seq_status.status.flag.badpt);
	input.pt = RTP_PT;
	input.ssrc = pj_htonl(0x87654321u);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	/* PJPROJECT 2.16 learns an unlocked changed SSRC. Its subsequent sequence
	 * update overwrites the transient badssrc flag, so validate the lasting
	 * session state rather than claiming that flag is reported. */
	CHECK_TRUE(test, rx.peer_ssrc == 0x87654321u);
	input.seq = pj_htons(5000);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.bad && seq_status.status.flag.outorder);
	input.seq = pj_htons(5001);
	pjmedia_rtp_session_update(&rx, &input, &seq_status);
	CHECK_TRUE(test, seq_status.status.flag.restart);

	printk("[Phase 8] RTP wrap/loss/reorder/duplicate/SSRC and bounds: PASSED\n");
	return 0;
}

static int test_rtcp(void)
{
	const char *test = "RTCP reports/statistics";
	pjmedia_rtcp_session session;
	pjmedia_rtcp_session peer;
	pjmedia_rtcp_session_setting setting;
	pjmedia_rtcp_rr_pkt report;
	pjmedia_rtcp_fb_nack nack = {321, 0x0005};
	pjmedia_rtcp_fb_nack parsed_nack;
	unsigned nack_count;
	void *packet;
	int packet_len;
	pj_uint8_t buffer[128];
	pj_size_t length;
	pjmedia_rtcp_sdes sdes;
	pj_str_t cname = pj_str("phase8@example");
	pj_str_t reason = pj_str("done");

	pjmedia_rtcp_session_setting_default(&setting);
	setting.name = "phase8-rtcp";
	setting.clock_rate = 8000;
	setting.samples_per_frame = 160;
	setting.ssrc = RTP_SSRC;
	setting.rtp_ts_base = 1000;
	pjmedia_rtcp_init2(&session, &setting);
	setting.name = "phase8-peer";
	setting.ssrc = 0x87654321u;
	pjmedia_rtcp_init2(&peer, &setting);

	pjmedia_rtcp_rx_rtp(&session, 100, 0, 160);
	pjmedia_rtcp_rx_rtp(&session, 101, 160, 160);
	pjmedia_rtcp_rx_rtp(&session, 102, 320, 160);
	pjmedia_rtcp_rx_rtp(&session, 103, 480, 160);
	pjmedia_rtcp_rx_rtp(&session, 105, 800, 160);
	pjmedia_rtcp_rx_rtp(&session, 107, 1120, 160);
	pjmedia_rtcp_rx_rtp(&session, 107, 1120, 160);
	pjmedia_rtcp_rx_rtp(&session, 106, 960, 160);
	CHECK_TRUE(test, session.stat.rx.pkt == 8);
	CHECK_TRUE(test, session.stat.rx.bytes == 1280);
	CHECK_TRUE(test, session.stat.rx.loss == 1);
	CHECK_TRUE(test, session.stat.rx.dup == 1);
	CHECK_TRUE(test, session.stat.rx.reorder == 1);

	session.peer_ssrc = 0x87654321u;
	session.jitter = 160;
	pjmedia_rtcp_build_rtcp(&session, &packet, &packet_len);
	CHECK_TRUE(test, packet_len == (int)sizeof(pjmedia_rtcp_rr_pkt));
	CHECK_TRUE(test, ((pjmedia_rtcp_rr_pkt *)packet)->common.pt == 201);
	CHECK_TRUE(test, pj_ntohl(((pjmedia_rtcp_rr_pkt *)packet)->rr.jitter) == 10);
	pjmedia_rtcp_rx_rtcp(&peer, packet, (pj_size_t)packet_len);

	pjmedia_rtcp_tx_rtp(&session, 160);
	pjmedia_rtcp_tx_rtp(&session, 80);
	session.stat.rtp_tx_last_ts = 4321;
	pjmedia_rtcp_build_rtcp(&session, &packet, &packet_len);
	CHECK_TRUE(test, packet_len == (int)sizeof(pjmedia_rtcp_sr_pkt));
	CHECK_TRUE(test, ((pjmedia_rtcp_sr_pkt *)packet)->common.pt == 200);
	CHECK_TRUE(test, pj_ntohl(((pjmedia_rtcp_sr_pkt *)packet)->sr.sender_pcount) == 2);
	CHECK_TRUE(test, pj_ntohl(((pjmedia_rtcp_sr_pkt *)packet)->sr.sender_bcount) == 240);
	CHECK_TRUE(test, pj_ntohl(((pjmedia_rtcp_sr_pkt *)packet)->sr.rtp_ts) == 4321);
	pjmedia_rtcp_rx_rtcp(&peer, packet, (pj_size_t)packet_len);
	CHECK_TRUE(test, peer.rx_lsr != 0);

	pj_bzero(&report, sizeof(report));
	report.common.version = 2;
	report.common.count = 1;
	report.common.pt = 201;
	report.common.length = pj_htons(7);
	report.rr.total_lost_0 = 2;
	report.rr.jitter = pj_htonl(16);
	pjmedia_rtcp_rx_rtcp(&session, &report, sizeof(report));
	CHECK_TRUE(test, session.stat.tx.loss == 2);
	CHECK_TRUE(test, session.stat.tx.jitter.n == 1);
	CHECK_TRUE(test, session.stat.tx.jitter.mean == 2000);

	pj_bzero(buffer, sizeof(buffer));
	length = 4;
	CHECK_EXPECTED(test, pjmedia_rtcp_build_rtcp_bye(&session, buffer,
							  &length, &reason),
		       PJ_ETOOSMALL);
	length = sizeof(buffer);
	CHECK_STATUS(test, pjmedia_rtcp_build_rtcp_bye(&session, buffer,
							 &length, &reason));
	CHECK_TRUE(test, length >= 12);
	pjmedia_rtcp_rx_rtcp(&peer, buffer, length);
	pj_bzero(&sdes, sizeof(sdes));
	sdes.cname = cname;
	length = sizeof(buffer);
	CHECK_STATUS(test, pjmedia_rtcp_build_rtcp_sdes(&session, buffer,
							  &length, &sdes));
	pjmedia_rtcp_rx_rtcp(&peer, buffer, length);
	CHECK_TRUE(test, peer.stat.peer_sdes.cname.slen == cname.slen);

	length = 8;
	CHECK_EXPECTED(test, pjmedia_rtcp_fb_build_nack(&session, buffer,
							  &length, 1, &nack),
		       PJ_ETOOSMALL);
	length = sizeof(buffer);
	CHECK_STATUS(test, pjmedia_rtcp_fb_build_nack(&session, buffer,
							 &length, 1, &nack));
	nack_count = 1;
	CHECK_STATUS(test, pjmedia_rtcp_fb_parse_nack(buffer, length,
							&nack_count, &parsed_nack));
	CHECK_TRUE(test, nack_count == 1 && parsed_nack.pid == nack.pid &&
			 parsed_nack.blp == nack.blp);
	pjmedia_rtcp_rx_rtcp(&session, buffer, 3);
	/* The outer compound parser owns malformed-length validation. Direct
	 * feedback parsers document a full feedback header as a precondition. */
	((pjmedia_rtcp_common *)buffer)->length = pj_htons(2);
	pjmedia_rtcp_rx_rtcp(&session, buffer, 8);
	((pjmedia_rtcp_common *)buffer)->length = pj_htons(0xffff);
	pjmedia_rtcp_rx_rtcp(&session, buffer, 12);
	CHECK_TRUE(test, session.stat.tx.loss == 2);
	CHECK_EXPECTED(test, pjmedia_rtcp_enable_xr(&session, PJ_TRUE), PJ_ENOTSUP);

	pjmedia_rtcp_fini(&peer);
	pjmedia_rtcp_fini(&session);
	printk("[Phase 8] RTCP SR/RR/loss/jitter/feedback and bounds: PASSED\n");
	return 0;
}

static int get_frame(pjmedia_jbuf *jb, pj_uint8_t output[FRAME_SIZE],
		     char *type, pj_uint32_t *ts, int *seq)
{
	pj_size_t size = FRAME_SIZE;
	pj_uint32_t bit_info = 0;

	pjmedia_jbuf_get_frame3(jb, output, &size, type, &bit_info, ts, seq);
	if (*type == PJMEDIA_JB_NORMAL_FRAME && size != FRAME_SIZE)
		return fail_condition("jitter-buffer frame", __LINE__,
				      "normal frame has full size");
	return 0;
}

static int test_jitter_buffer(pj_pool_t *pool)
{
	const char *test = "jitter buffer";
	pjmedia_jbuf *jb = NULL;
	pjmedia_jb_state state;
	pj_str_t name = pj_str("phase8-jbuf");
	pj_uint8_t frame[6] = {1, 2, 3, 4, 5, 6};
	pj_uint8_t output[FRAME_SIZE] = {0};
	const void *peek = NULL;
	pj_size_t size = 0;
	pj_uint32_t bit_info = 0;
	pj_uint32_t ts = 0;
	int seq = 0;
	char type = 0;
	pj_bool_t discarded = PJ_FALSE;
	unsigned i;

	CHECK_STATUS(test, pjmedia_jbuf_create(pool, &name, FRAME_SIZE, 20, 4, &jb));
	CHECK_STATUS(test, pjmedia_jbuf_set_ptime(jb, 20));
	CHECK_STATUS(test, pjmedia_jbuf_set_ptime2(jb, 40, 2));
	CHECK_STATUS(test, pjmedia_jbuf_set_fixed(jb, 2));
	CHECK_STATUS(test, pjmedia_jbuf_get_state(jb, &state));
	CHECK_TRUE(test, state.prefetch == 2 && state.max_count == 4);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_ZERO_PREFETCH_FRAME);
	pjmedia_jbuf_put_frame3(jb, frame, FRAME_SIZE, 0x55, 65535, 1000,
				 &discarded);
	CHECK_TRUE(test, !discarded);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_ZERO_PREFETCH_FRAME);
	pjmedia_jbuf_put_frame3(jb, frame, FRAME_SIZE, 0x55, 65536, 1160,
				 &discarded);
	CHECK_TRUE(test, !discarded);
	pjmedia_jbuf_peek_frame(jb, 0, &peek, &size, &type, &bit_info, &ts, &seq);
	CHECK_TRUE(test, type == PJMEDIA_JB_NORMAL_FRAME && size == FRAME_SIZE);
	CHECK_TRUE(test, seq == 65535 && ts == 1000 && bit_info == 0x55);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_NORMAL_FRAME && seq == 65535);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_NORMAL_FRAME && seq == 65536);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_ZERO_EMPTY_FRAME);

	CHECK_STATUS(test, pjmedia_jbuf_reset(jb));
	CHECK_STATUS(test, pjmedia_jbuf_set_fixed(jb, 0));
	pjmedia_jbuf_put_frame(jb, frame, FRAME_SIZE, 10);
	pjmedia_jbuf_put_frame(jb, frame, FRAME_SIZE, 12);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_NORMAL_FRAME && seq == 10);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_MISSING_FRAME && seq == 11);
	if (get_frame(jb, output, &type, &ts, &seq) != 0)
		return -1;
	CHECK_TRUE(test, type == PJMEDIA_JB_NORMAL_FRAME && seq == 12);
	pjmedia_jbuf_put_frame2(jb, frame, FRAME_SIZE, 0, 11, &discarded);
	CHECK_TRUE(test, discarded);

	CHECK_STATUS(test, pjmedia_jbuf_reset(jb));
	CHECK_STATUS(test, pjmedia_jbuf_set_discard(jb, PJMEDIA_JB_DISCARD_NONE));
	for (i = 1; i <= 4; ++i)
		pjmedia_jbuf_put_frame(jb, frame, FRAME_SIZE, (int)i);
	CHECK_TRUE(test, pjmedia_jbuf_is_full(jb));
	pjmedia_jbuf_put_frame(jb, frame, sizeof(frame), 5);
	CHECK_STATUS(test, pjmedia_jbuf_get_state(jb, &state));
	CHECK_TRUE(test, state.size == 4 && state.discard >= 1);
	CHECK_TRUE(test, state.size <= state.max_count);
	if (state.size > metrics.max_jbuf_frames)
		metrics.max_jbuf_frames = state.size;
	CHECK_TRUE(test, pjmedia_jbuf_remove_frame(jb, 2) == 2);
	CHECK_STATUS(test, pjmedia_jbuf_set_adaptive(jb, 1, 0, 3));
	CHECK_STATUS(test, pjmedia_jbuf_set_discard(jb, PJMEDIA_JB_DISCARD_STATIC));
	CHECK_STATUS(test, pjmedia_jbuf_set_min_delay(jb, 200));
	CHECK_STATUS(test, pjmedia_jbuf_get_state(jb, &state));
	CHECK_TRUE(test, state.min_delay_set == 2);
	CHECK_STATUS(test, pjmedia_jbuf_reset(jb));
	CHECK_STATUS(test, pjmedia_jbuf_get_state(jb, &state));
	CHECK_TRUE(test, state.size == 0);
	CHECK_STATUS(test, pjmedia_jbuf_destroy(jb));
	printk("[Phase 8] jitter buffer prefetch/underflow/overflow/reorder/reset: PASSED\n");
	return 0;
}

static void benchmark_primitives(void)
{
	pjmedia_rtp_seq_session seq;
	pjmedia_rtp_status status;
	pj_uint64_t start;
	pj_uint64_t elapsed;
	unsigned checksum = 0;
	unsigned i;

	pjmedia_rtp_seq_init(&seq, 0);
	start = k_uptime_get();
	for (i = 0; i < BENCH_ITERATIONS; ++i) {
		pjmedia_rtp_seq_update(&seq, (pj_uint16_t)i, &status);
		checksum += status.status.value + status.diff;
	}
	elapsed = k_uptime_get() - start;
	benchmark_checksum = checksum;
	printk("[Phase 8] QEMU fixed-point sequence cost: %llu ms (%u updates)\n",
	       (unsigned long long)elapsed, BENCH_ITERATIONS);
}

static int run_lifecycle(unsigned lifecycle)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool = NULL;
	pj_status_t status;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS)
		goto shutdown;
	pj_caching_pool_init(&caching_pool, NULL, 0);
	active_caching_pool = &caching_pool;
	caching_pool.factory.on_block_alloc = phase8_on_block_alloc;
	caching_pool.factory.on_block_free = phase8_on_block_free;
	pool = pj_pool_create(&caching_pool.factory, "phase8-test", 16384, 8192,
			      NULL);
	if (pool == NULL) {
		status = PJ_ENOMEM;
		goto destroy_factory;
	}
	if (test_rtp() != 0 || test_rtcp() != 0 ||
	    test_jitter_buffer(pool) != 0)
		goto release_pool;
	if (lifecycle == 1)
		benchmark_primitives();
	if (pj_pool_get_used_size(pool) > metrics.peak_pool_used)
		metrics.peak_pool_used = pj_pool_get_used_size(pool);
	result = 0;

release_pool:
	pj_pool_release(pool);
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
		printk("[Phase 8] lifecycle %u primitive teardown: PASSED\n",
		       lifecycle);
	return result;
}

int phase8_packet_run(void)
{
	size_t stack_unused = 0;
	unsigned lifecycle;

	pj_bzero(&metrics, sizeof(metrics));
	printk("[Phase 8] RTP/RTCP/jitter-buffer validation (%u lifecycles)\n",
	       PHASE8_LIFECYCLES);
	for (lifecycle = 1; lifecycle <= PHASE8_LIFECYCLES; ++lifecycle) {
		if (run_lifecycle(lifecycle) != 0) {
			printk("PHASE 8 RESULT: FAILED at lifecycle %u\n", lifecycle);
			return 1;
		}
	}
	if (k_thread_stack_space_get(k_current_get(), &stack_unused) != 0)
		return fail_condition("stack watermark", __LINE__,
				      "k_thread_stack_space_get success");
	printk("[Phase 8] resources: PJ blocks peak=%u/%u B, pool used peak=%u B, jbuf=%u frames\n",
	       metrics.peak_blocks, (unsigned)metrics.peak_bytes,
	       (unsigned)metrics.peak_pool_used, metrics.max_jbuf_frames);
	printk("[Phase 8] main stack: configured=%u B, used<=%u B, unused=%u B\n",
	       (unsigned)CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - stack_unused),
	       (unsigned)stack_unused);
	printk("PHASE 8 RESULT: PASSED (3 socket-free RTP/RTCP/jitter lifecycles)\n");
	return 0;
}
