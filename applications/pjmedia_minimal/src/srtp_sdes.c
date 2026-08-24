#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/sdp.h>
#include <pjmedia/transport.h>
#include <pjmedia/transport_loop.h>
#include <pjmedia/transport_srtp.h>

static int fail_status(const char *case_name, int line, pj_status_t status)
{
	char message[PJ_ERR_MSG_SIZE];

	pj_strerror(status, message, sizeof(message));
	printk("[SRTP SDES] FAIL %s line %d: status=%d (%s)\n", case_name,
	       line, status, message);
	return -1;
}

static int fail_condition(const char *case_name, int line,
			  const char *condition)
{
	printk("[SRTP SDES] FAIL %s line %d: %s\n", case_name, line,
	       condition);
	return -1;
}

#define REQUIRE_STATUS(name, expression)                                     \
	do {                                                                     \
		pj_status_t status_ = (expression);                                 \
		if (status_ != PJ_SUCCESS)                                          \
			return fail_status((name), __LINE__, status_);                 \
	} while (0)

#define REQUIRE(name, condition)                                             \
	do {                                                                     \
		if (!(condition))                                                   \
			return fail_condition((name), __LINE__, #condition);          \
	} while (0)

static pjmedia_sdp_session *make_sdp(pj_pool_t *pool, const char *transport,
				     const char *crypto_value)
{
	pjmedia_sdp_session *session;
	pjmedia_sdp_media *media;

	session = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_session);
	session->origin.user = pj_str("zephyr");
	session->origin.id = 1;
	session->origin.version = 1;
	session->origin.net_type = pj_str("IN");
	session->origin.addr_type = pj_str("IP4");
	session->origin.addr = pj_str("127.0.0.1");
	session->name = pj_str("srtp-sdes");
	media = PJ_POOL_ZALLOC_T(pool, pjmedia_sdp_media);
	media->desc.media = pj_str("audio");
	media->desc.port = 4000;
	media->desc.port_count = 1;
	media->desc.transport = pj_str((char *)transport);
	media->desc.fmt_count = 1;
	media->desc.fmt[0] = pj_str("0");
	if (crypto_value != NULL) {
		pj_str_t value = pj_str((char *)crypto_value);

		media->attr[media->attr_count++] =
			pjmedia_sdp_attr_create(pool, "crypto", &value);
	}
	session->media[session->media_count++] = media;
	return session;
}

static void init_setting(pjmedia_srtp_setting *setting, char key[30])
{
	pjmedia_srtp_setting_default(setting);
	setting->use = PJMEDIA_SRTP_MANDATORY;
	setting->close_member_tp = PJ_TRUE;
	setting->crypto_count = 1;
	setting->crypto[0].name = pj_str("AES_CM_128_HMAC_SHA1_80");
	setting->crypto[0].key.ptr = key;
	setting->crypto[0].key.slen = 30;
	setting->keying_count = 1;
	setting->keying[0] = PJMEDIA_SRTP_KEYING_SDES;
}

static pj_status_t create_transport(pjmedia_endpt *endpt, char key[30],
				    pjmedia_transport **transport)
{
	pjmedia_transport *loop = NULL;
	pjmedia_srtp_setting setting;
	pj_status_t status;

	status = pjmedia_transport_loop_create(endpt, &loop);
	if (status != PJ_SUCCESS)
		return status;
	init_setting(&setting, key);
	status = pjmedia_transport_srtp_create(endpt, loop, &setting, transport);
	if (status != PJ_SUCCESS)
		pjmedia_transport_close(loop);
	return status;
}

static int test_offer_answer(pjmedia_endpt *endpt, pj_pool_t *pool,
			     char key_a[30], char key_b[30])
{
	const char *name = "mandatory offer/answer";
	pjmedia_transport *offerer = NULL;
	pjmedia_transport *answerer = NULL;
	pjmedia_sdp_session *offer = make_sdp(pool, "RTP/AVP", NULL);
	pjmedia_sdp_session *answer = make_sdp(pool, "RTP/AVP", NULL);
	pjmedia_sdp_attr *offer_crypto;
	pjmedia_sdp_attr *answer_crypto;
	pj_str_t crypto_name = pj_str("crypto");
	int result = -1;

	REQUIRE_STATUS(name, create_transport(endpt, key_a, &offerer));
	REQUIRE_STATUS(name, create_transport(endpt, key_b, &answerer));
	REQUIRE_STATUS(name, pjmedia_transport_media_create(offerer, pool, 0,
							     NULL, 0));
	REQUIRE_STATUS(name, pjmedia_transport_encode_sdp(offerer, pool, offer,
							   NULL, 0));
	offer_crypto = pjmedia_sdp_media_find_attr(offer->media[0], &crypto_name,
						   NULL);
	REQUIRE(name, pj_strcmp2(&offer->media[0]->desc.transport,
				 "RTP/SAVP") == 0);
	REQUIRE(name, offer_crypto != NULL);
	REQUIRE(name, offer_crypto->value.slen == 73);
	REQUIRE(name, pj_strncmp2(&offer_crypto->value,
				  "1 AES_CM_128_HMAC_SHA1_80 inline:", 33) == 0);

	REQUIRE_STATUS(name, pjmedia_transport_media_create(answerer, pool, 0,
							      offer, 0));
	REQUIRE_STATUS(name, pjmedia_transport_encode_sdp(answerer, pool, answer,
							   offer, 0));
	answer_crypto = pjmedia_sdp_media_find_attr(answer->media[0], &crypto_name,
						    NULL);
	REQUIRE(name, pj_strcmp2(&answer->media[0]->desc.transport,
				 "RTP/SAVP") == 0);
	REQUIRE(name, answer_crypto != NULL);
	REQUIRE(name, answer_crypto->value.slen == 73);
	REQUIRE(name, pj_strcmp(&offer_crypto->value, &answer_crypto->value) != 0);
	REQUIRE_STATUS(name, pjmedia_transport_media_start(answerer, pool, answer,
							    offer, 0));
	REQUIRE_STATUS(name, pjmedia_transport_media_start(offerer, pool, offer,
							   answer, 0));
	REQUIRE_STATUS(name, pjmedia_transport_media_stop(offerer));
	REQUIRE_STATUS(name, pjmedia_transport_media_stop(answerer));
	result = 0;
	pjmedia_transport_close(offerer);
	pjmedia_transport_close(answerer);
	return result;
}

static int expect_rejected(pjmedia_endpt *endpt, pj_pool_t *pool,
			   char key[30], const char *case_name,
			   const char *transport_name, const char *crypto_value)
{
	pjmedia_transport *answerer = NULL;
	pjmedia_sdp_session *remote =
		make_sdp(pool, transport_name, crypto_value);
	pjmedia_sdp_session *local = make_sdp(pool, "RTP/AVP", NULL);
	pj_status_t status;

	REQUIRE_STATUS(case_name, create_transport(endpt, key, &answerer));
	status = pjmedia_transport_media_create(answerer, pool, 0, remote, 0);
	if (status == PJ_SUCCESS)
		status = pjmedia_transport_encode_sdp(answerer, pool, local, remote, 0);
	pjmedia_transport_close(answerer);
	REQUIRE(case_name, status != PJ_SUCCESS);
	return 0;
}

int srtp_sdes_run(void)
{
	pj_caching_pool caching_pool;
	pj_pool_t *pool = NULL;
	pj_pool_t *ioqueue_pool = NULL;
	pj_ioqueue_t *ioqueue = NULL;
	pjmedia_endpt *endpt = NULL;
	char key_a[30];
	char key_b[30];
	pj_status_t status;
	int result = -1;

	for (unsigned i = 0; i < sizeof(key_a); ++i) {
		key_a[i] = (char)(i * 7U + 3U);
		key_b[i] = (char)(0xa5U ^ i * 11U);
	}
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS)
		goto shutdown;
	pj_caching_pool_init(&caching_pool, NULL, 0);
	ioqueue_pool = pj_pool_create(&caching_pool.factory, "sdes-ioq", 32768,
				      32768, NULL);
	pool = pj_pool_create(&caching_pool.factory, "sdes-test", 16384, 16384,
			      NULL);
	if (ioqueue_pool == NULL || pool == NULL)
		goto destroy_factory;
	status = pj_ioqueue_create(ioqueue_pool, 4, &ioqueue);
	if (status != PJ_SUCCESS)
		goto destroy_factory;
	status = pjmedia_endpt_create2(&caching_pool.factory, ioqueue, 0, &endpt);
	if (status != PJ_SUCCESS)
		goto destroy_ioqueue;

	if (test_offer_answer(endpt, pool, key_a, key_b) != 0 ||
	    expect_rejected(endpt, pool, key_b, "downgrade", "RTP/AVP",
			    NULL) != 0 ||
	    expect_rejected(endpt, pool, key_b, "missing crypto", "RTP/SAVP",
			    NULL) != 0 ||
	    expect_rejected(endpt, pool, key_b, "malformed base64", "RTP/SAVP",
			    "1 AES_CM_128_HMAC_SHA1_80 inline:AQI%") != 0 ||
	    expect_rejected(endpt, pool, key_b, "short key", "RTP/SAVP",
			    "1 AES_CM_128_HMAC_SHA1_80 inline:AQID") != 0 ||
	    expect_rejected(endpt, pool, key_b, "unsupported suite", "RTP/SAVP",
			    "1 AES_999_CM_HMAC_SHA1_80 inline:AQID") != 0 ||
	    expect_rejected(endpt, pool, key_b, "malformed tag", "RTP/SAVP",
			    "01 AES_CM_128_HMAC_SHA1_80 inline:AQID") != 0)
		goto destroy_endpoint;
	result = 0;

destroy_endpoint:
	pjmedia_endpt_destroy2(endpt);
destroy_ioqueue:
	pj_ioqueue_destroy(ioqueue);
destroy_factory:
	if (pool != NULL)
		pj_pool_release(pool);
	if (ioqueue_pool != NULL)
		pj_pool_release(ioqueue_pool);
	pj_bzero(key_a, sizeof(key_a));
	pj_bzero(key_b, sizeof(key_b));
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("SRTP SDES RESULT: %s (mandatory RTP/SAVP; strict suite)\n",
	       result == 0 ? "PASSED" : "FAILED");
	return result == 0 ? 0 : 1;
}
