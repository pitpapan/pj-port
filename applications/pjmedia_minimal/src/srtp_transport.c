#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/transport.h>
#include <pjmedia/transport_srtp.h>
#include <pjmedia/transport_udp.h>

#define WAIT_MS 2000

static pj_ioqueue_t *test_ioqueue;

static const pj_uint8_t rtp_packet[] = {
	0x80, 0x00, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
	0x11, 0x22, 0x33, 0x44, 1, 2, 3, 4, 5, 6, 7, 8,
};
static const pj_uint8_t rtcp_packet[] = {
	0x80, 0xc9, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44,
};

struct receiver {
	atomic_t rtp_count;
	atomic_t rtcp_count;
	atomic_t bad_packet;
};

static int fail_status(int line, pj_status_t status)
{
	char message[PJ_ERR_MSG_SIZE];

	pj_strerror(status, message, sizeof(message));
	printk("[SRTP transport] FAIL line %d: status=%d (%s)\n", line,
	       status, message);
	return -1;
}

#define CHECK_STATUS(expression)                                             \
	do {                                                                    \
		pj_status_t status_ = (expression);                                \
		if (status_ != PJ_SUCCESS)                                         \
			return fail_status(__LINE__, status_);                         \
	} while (0)

#define CHECK_TRUE(condition)                                                \
	do {                                                                    \
		if (!(condition)) {                                                 \
			printk("[SRTP transport] FAIL line %d: %s\n", __LINE__,      \
			       #condition);                                             \
			return -1;                                                     \
		}                                                                   \
	} while (0)

static void on_rtp(void *user_data, void *packet, pj_ssize_t size)
{
	struct receiver *receiver = user_data;

	if (size == sizeof(rtp_packet) &&
	    pj_memcmp(packet, rtp_packet, sizeof(rtp_packet)) == 0)
		atomic_inc(&receiver->rtp_count);
	else
		atomic_inc(&receiver->bad_packet);
}

static void on_rtcp(void *user_data, void *packet, pj_ssize_t size)
{
	struct receiver *receiver = user_data;

	if (size == sizeof(rtcp_packet) &&
	    pj_memcmp(packet, rtcp_packet, sizeof(rtcp_packet)) == 0)
		atomic_inc(&receiver->rtcp_count);
	else
		atomic_inc(&receiver->bad_packet);
}

static int wait_for(atomic_t *counter, atomic_val_t target)
{
	for (unsigned elapsed = 0; elapsed < WAIT_MS; elapsed += 10) {
		pj_time_val timeout = {0, 0};

		if (atomic_get(counter) >= target)
			return 0;
		(void)pj_ioqueue_poll(test_ioqueue, &timeout);
		k_msleep(10);
	}
	return -1;
}

static void poll_for(unsigned duration_ms)
{
	for (unsigned elapsed = 0; elapsed < duration_ms; elapsed += 10) {
		pj_time_val timeout = {0, 0};

		(void)pj_ioqueue_poll(test_ioqueue, &timeout);
		k_msleep(10);
	}
}

static void init_attach(pjmedia_transport_attach_param *param,
			struct receiver *receiver,
			const pjmedia_transport_info *peer)
{
	pj_bzero(param, sizeof(*param));
	param->media_type = PJMEDIA_TYPE_AUDIO;
	param->user_data = receiver;
	pj_sockaddr_cp(&param->rem_addr, &peer->sock_info.rtp_addr_name);
	pj_sockaddr_cp(&param->rem_rtcp, &peer->sock_info.rtcp_addr_name);
	param->addr_len = sizeof(pj_sockaddr_in);
	param->rtp_cb = on_rtp;
	param->rtcp_cb = on_rtcp;
}

static void init_crypto(pjmedia_srtp_crypto *crypto, char *key)
{
	pj_bzero(crypto, sizeof(*crypto));
	crypto->name = pj_str("AES_CM_128_HMAC_SHA1_80");
	crypto->key.ptr = key;
	crypto->key.slen = 30;
}

static int run_transport_test(pjmedia_endpt *endpt)
{
	char key_a[30];
	char key_b[30];
	pjmedia_transport *udp_a = NULL;
	pjmedia_transport *udp_b = NULL;
	pjmedia_transport *srtp_a = NULL;
	pjmedia_transport *srtp_b = NULL;
	pjmedia_transport_info info_a;
	pjmedia_transport_info info_b;
	pjmedia_transport_attach_param attach;
	pjmedia_srtp_setting setting;
	pjmedia_srtp_crypto crypto_a;
	pjmedia_srtp_crypto crypto_b;
	struct receiver receive_a = {0};
	struct receiver receive_b = {0};
	pj_str_t loopback = pj_str("127.0.0.1");
	int result = -1;

	for (unsigned i = 0; i < sizeof(key_a); ++i) {
		key_a[i] = (char)(3U + i * 7U);
		key_b[i] = (char)(0xa5U ^ i * 11U);
	}
	init_crypto(&crypto_a, key_a);
	init_crypto(&crypto_b, key_b);
	pjmedia_srtp_setting_default(&setting);
	setting.use = PJMEDIA_SRTP_MANDATORY;
	setting.close_member_tp = PJ_TRUE;
	setting.crypto_count = 1;
	setting.crypto[0].name = crypto_a.name;
	setting.keying_count = 0;

	CHECK_STATUS(pjmedia_transport_udp_create2(endpt, "srtp-udp-a",
						    &loopback, 41000,
						    PJMEDIA_UDP_NO_SRC_ADDR_CHECKING,
						    &udp_a));
	CHECK_STATUS(pjmedia_transport_udp_create2(endpt, "srtp-udp-b",
						    &loopback, 41002,
						    PJMEDIA_UDP_NO_SRC_ADDR_CHECKING,
						    &udp_b));
	CHECK_STATUS(pjmedia_transport_srtp_create(endpt, udp_a, &setting,
						   &srtp_a));
	udp_a = NULL;
	CHECK_STATUS(pjmedia_transport_srtp_create(endpt, udp_b, &setting,
						   &srtp_b));
	udp_b = NULL;
	CHECK_STATUS(pjmedia_transport_srtp_start(srtp_a, &crypto_a, &crypto_b));
	CHECK_STATUS(pjmedia_transport_srtp_start(srtp_b, &crypto_b, &crypto_a));

	pjmedia_transport_info_init(&info_a);
	pjmedia_transport_info_init(&info_b);
	CHECK_STATUS(pjmedia_transport_get_info(srtp_a, &info_a));
	CHECK_STATUS(pjmedia_transport_get_info(srtp_b, &info_b));
	init_attach(&attach, &receive_a, &info_b);
	CHECK_STATUS(pjmedia_transport_attach2(srtp_a, &attach));
	init_attach(&attach, &receive_b, &info_a);
	CHECK_STATUS(pjmedia_transport_attach2(srtp_b, &attach));
	CHECK_STATUS(pjmedia_transport_media_start(
		pjmedia_transport_srtp_get_member(srtp_a), NULL, NULL, NULL, 0));
	CHECK_STATUS(pjmedia_transport_media_start(
		pjmedia_transport_srtp_get_member(srtp_b), NULL, NULL, NULL, 0));

	CHECK_STATUS(pjmedia_transport_send_rtp(srtp_a, rtp_packet,
						 sizeof(rtp_packet)));
	CHECK_TRUE(wait_for(&receive_b.rtp_count, 1) == 0);
	CHECK_STATUS(pjmedia_transport_send_rtcp(srtp_b, rtcp_packet,
						  sizeof(rtcp_packet)));
	CHECK_TRUE(wait_for(&receive_a.rtcp_count, 1) == 0);
	CHECK_TRUE(atomic_get(&receive_a.bad_packet) == 0 &&
		   atomic_get(&receive_b.bad_packet) == 0);

	/* Bypass A's SRTP adapter while retaining the expected UDP source. The
	 * receiving adapter must silently reject this unauthenticated RTP. */
	CHECK_STATUS(pjmedia_transport_send_rtp(
		pjmedia_transport_srtp_get_member(srtp_a), rtp_packet,
		sizeof(rtp_packet)));
	poll_for(100);
	CHECK_TRUE(atomic_get(&receive_b.rtp_count) == 1 &&
		   atomic_get(&receive_b.bad_packet) == 0);

	result = 0;
	pjmedia_transport_detach(srtp_a, &receive_a);
	pjmedia_transport_detach(srtp_b, &receive_b);
	CHECK_STATUS(pjmedia_transport_close(srtp_a));
	srtp_a = NULL;
	CHECK_STATUS(pjmedia_transport_close(srtp_b));
	srtp_b = NULL;
	pj_bzero(key_a, sizeof(key_a));
	pj_bzero(key_b, sizeof(key_b));
	CHECK_TRUE(key_a[0] == 0 && key_b[0] == 0);
	return result;
}

int srtp_transport_run(void)
{
	pj_caching_pool caching_pool;
	pjmedia_endpt *endpt = NULL;
	pj_pool_t *ioqueue_pool = NULL;
	pj_status_t status;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status(__LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status(__LINE__, status);
		goto shutdown;
	}
	pj_caching_pool_init(&caching_pool, NULL, 0);
	ioqueue_pool = pj_pool_create(&caching_pool.factory, "srtp-ioq", 32768,
				      32768, NULL);
	if (ioqueue_pool == NULL) {
		status = PJ_ENOMEM;
		fail_status(__LINE__, status);
		goto destroy_pool;
	}
	status = pj_ioqueue_create(ioqueue_pool, 8, &test_ioqueue);
	if (status != PJ_SUCCESS) {
		fail_status(__LINE__, status);
		goto destroy_ioqueue_pool;
	}
	status = pjmedia_endpt_create2(&caching_pool.factory, test_ioqueue, 0,
				       &endpt);
	if (status != PJ_SUCCESS) {
		fail_status(__LINE__, status);
		goto destroy_ioqueue;
	}
	result = run_transport_test(endpt);
	status = pjmedia_endpt_destroy2(endpt);
	if (status != PJ_SUCCESS) {
		fail_status(__LINE__, status);
		result = -1;
	}
	endpt = NULL;
destroy_ioqueue:
	pj_ioqueue_destroy(test_ioqueue);
	test_ioqueue = NULL;
destroy_ioqueue_pool:
	pj_pool_release(ioqueue_pool);
	if (result == 0 &&
	    (caching_pool.used_count != 0 || caching_pool.capacity != 0)) {
		printk("[SRTP transport] FAIL pool teardown: used=%u capacity=%u\n",
		       (unsigned)caching_pool.used_count,
		       (unsigned)caching_pool.capacity);
		result = -1;
	}
destroy_pool:
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("SRTP TRANSPORT RESULT: %s (UDP; AES_CM_128_HMAC_SHA1_80)\n",
	       result == 0 ? "PASSED" : "FAILED");
	return result == 0 ? 0 : 1;
}
