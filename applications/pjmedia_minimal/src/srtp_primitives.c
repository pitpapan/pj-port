#include <srtp.h>

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/printk.h>

static int failures;

#define CHECK(condition) do {                                                  \
	if (!(condition)) {                                                        \
		printk("[SRTP] FAIL line %d: %s\n", __LINE__, #condition);            \
		++failures;                                                             \
	}                                                                          \
} while (0)

static void make_policy(srtp_policy_t *value, uint8_t *key, uint32_t ssrc)
{
	memset(value, 0, sizeof(*value));
	srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&value->rtp);
	srtp_crypto_policy_set_rtcp_default(&value->rtcp);
	value->ssrc.type = ssrc_specific;
	value->ssrc.value = ssrc;
	value->key = key;
	value->window_size = 128;
}

int srtp_primitives_run(void)
{
	static const uint8_t plain_rtp[] = {
		0x80, 0x00, 0x12, 0x34, 0, 0, 0, 0xa0,
		0x11, 0x22, 0x33, 0x44, 1, 2, 3, 4, 5, 6, 7, 8
	};
	static const uint8_t plain_rtcp[] = {
		0x80, 200, 0, 1, 0x11, 0x22, 0x33, 0x44
	};
	uint8_t key[30];
	uint8_t packet[128];
	uint8_t protected_copy[128];
	srtp_policy_t sender_policy;
	srtp_policy_t receiver_policy;
	srtp_t sender = NULL;
	srtp_t receiver = NULL;
	int length;

	for (unsigned i = 0; i < sizeof(key); ++i)
		key[i] = (uint8_t)(i * 7U + 3U);
	CHECK(srtp_init() == srtp_err_status_ok);
	make_policy(&sender_policy, key, 0x11223344U);
	make_policy(&receiver_policy, key, 0x11223344U);
	CHECK(srtp_create(&sender, &sender_policy) == srtp_err_status_ok);
	CHECK(srtp_create(&receiver, &receiver_policy) == srtp_err_status_ok);

	memcpy(packet, plain_rtp, sizeof(plain_rtp));
	length = sizeof(plain_rtp);
	CHECK(srtp_protect(sender, packet, &length) == srtp_err_status_ok);
	CHECK(length == (int)sizeof(plain_rtp) + 10);
	CHECK(memcmp(packet + 12, plain_rtp + 12, sizeof(plain_rtp) - 12) != 0);
	memcpy(protected_copy, packet, length);
	CHECK(srtp_unprotect(receiver, packet, &length) == srtp_err_status_ok);
	CHECK(length == (int)sizeof(plain_rtp));
	CHECK(memcmp(packet, plain_rtp, sizeof(plain_rtp)) == 0);
	length = sizeof(plain_rtp) + 10;
	CHECK(srtp_unprotect(receiver, protected_copy, &length) ==
	      srtp_err_status_replay_fail);

	memcpy(packet, plain_rtp, sizeof(plain_rtp));
	packet[3] = 0x35;
	length = sizeof(plain_rtp);
	CHECK(srtp_protect(sender, packet, &length) == srtp_err_status_ok);
	packet[length - 1] ^= 0x80;
	CHECK(srtp_unprotect(receiver, packet, &length) == srtp_err_status_auth_fail);

	memcpy(packet, plain_rtcp, sizeof(plain_rtcp));
	length = sizeof(plain_rtcp);
	CHECK(srtp_protect_rtcp(sender, packet, &length) == srtp_err_status_ok);
	CHECK(length > (int)sizeof(plain_rtcp));
	/* This minimal RR contains only the clear SRTCP header and SSRC. */
	CHECK(srtp_unprotect_rtcp(receiver, packet, &length) == srtp_err_status_ok);
	CHECK(length == (int)sizeof(plain_rtcp));
	CHECK(memcmp(packet, plain_rtcp, sizeof(plain_rtcp)) == 0);

	CHECK(srtp_dealloc(sender) == srtp_err_status_ok);
	CHECK(srtp_dealloc(receiver) == srtp_err_status_ok);
	CHECK(srtp_shutdown() == srtp_err_status_ok);
	memset(key, 0, sizeof(key));
	CHECK(key[0] == 0 && key[sizeof(key) - 1] == 0);
	printk("SRTP PRIMITIVE RESULT: %s (AES_CM_128_HMAC_SHA1_80)\n",
	       failures == 0 ? "PASSED" : "FAILED");
	return failures == 0 ? 0 : 1;
}
