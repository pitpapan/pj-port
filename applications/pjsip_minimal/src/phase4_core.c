#include <zephyr/sys/printk.h>

#include <pjsip.h>

/* Configuration and pool values audited for the first embedded profile. */
_Static_assert(PJ_HAS_IPV6 == 0, "Phase 4 requires IPv6 to remain disabled");
_Static_assert(PJSIP_HAS_TLS_TRANSPORT == 0, "TLS must remain excluded");
_Static_assert(PJSIP_HAS_DIGEST_AKA_AUTH == 0, "Digest AKA must remain excluded");
_Static_assert(PJSIP_HAS_RESOLVER == 0, "full DNS/SRV resolver is deferred");
_Static_assert(PJSIP_MAX_TSX_COUNT == 31, "unexpected transaction limit");
_Static_assert(PJSIP_MAX_DIALOG_COUNT == 15, "unexpected dialog limit");
_Static_assert(PJSIP_MAX_TRANSPORTS == 16, "unexpected transport limit");
_Static_assert(PJSIP_TPMGR_HTABLE_SIZE == 15, "unexpected transport hash size");
_Static_assert(PJSIP_MAX_MODULE == 16, "unexpected module limit");
_Static_assert(PJSIP_MAX_PKT_LEN == 4000, "unexpected packet limit");
_Static_assert(PJSIP_MAX_TIMER_COUNT == 128, "unexpected timer capacity");
_Static_assert(PJSIP_MAX_NET_EVENTS == 1, "unexpected network event limit");
_Static_assert(PJSIP_MAX_TIMED_OUT_ENTRIES == 10, "unexpected timer poll limit");
_Static_assert(PJSIP_MAX_TRANSPORTS <= PJ_IOQUEUE_MAX_HANDLES,
	       "PJSIP transport count exceeds the PJLIB ioqueue capacity");

_Static_assert(PJSIP_POOL_LEN_ENDPT == 16000, "unexpected endpoint pool size");
_Static_assert(PJSIP_POOL_INC_ENDPT == 4000, "unexpected endpoint pool increment");
_Static_assert(PJSIP_POOL_RDATA_LEN == 4000, "unexpected receive pool size");
_Static_assert(PJSIP_POOL_RDATA_INC == 4000, "unexpected receive pool increment");
_Static_assert(PJSIP_POOL_LEN_TRANSPORT == 512, "unexpected transport pool size");
_Static_assert(PJSIP_POOL_INC_TRANSPORT == 512, "unexpected transport pool increment");
_Static_assert(PJSIP_POOL_LEN_TDATA == 4000, "unexpected transmit pool size");
_Static_assert(PJSIP_POOL_INC_TDATA == 4000, "unexpected transmit pool increment");
_Static_assert(PJSIP_POOL_LEN_UA == 512, "unexpected core UA pool size");
_Static_assert(PJSIP_POOL_INC_UA == 512, "unexpected core UA pool increment");
_Static_assert(PJSIP_POOL_TSX_LAYER_LEN == 512, "unexpected transaction-layer pool size");
_Static_assert(PJSIP_POOL_TSX_LAYER_INC == 512,
	       "unexpected transaction-layer pool increment");
_Static_assert(PJSIP_POOL_TSX_LEN == 1536, "unexpected transaction pool size");
_Static_assert(PJSIP_POOL_TSX_INC == 256, "unexpected transaction pool increment");
_Static_assert(PJSIP_POOL_LEN_USER_AGENT == 1024, "unexpected user-agent pool size");
_Static_assert(PJSIP_POOL_INC_USER_AGENT == 1024,
	       "unexpected user-agent pool increment");
_Static_assert(PJSIP_POOL_LEN_DIALOG == 4000, "unexpected dialog pool size");
_Static_assert(PJSIP_POOL_INC_DIALOG == 4000, "unexpected dialog pool increment");

int phase4_core_run(void)
{
	const pjsip_cfg_t *cfg = pjsip_cfg();

	/* pjsip_cfg() links the core configuration object without initializing an
	 * endpoint. Endpoint lifecycle validation starts in Phase 5.
	 */
	if (cfg->tsx.max_count != PJSIP_MAX_TSX_COUNT) {
		printk("Phase 4 FAIL: transaction limit mismatch\n");
		return 1;
	}

	printk("Phase 4 PASS: PJSIP core linked (tsx=%u dialogs=%u transports=%u pkt=%u)\n",
	       cfg->tsx.max_count, (unsigned int)PJSIP_MAX_DIALOG_COUNT,
	       (unsigned int)PJSIP_MAX_TRANSPORTS, (unsigned int)PJSIP_MAX_PKT_LEN);
	return 0;
}
