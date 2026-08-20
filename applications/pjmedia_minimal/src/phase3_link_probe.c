#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjmedia/sdp_neg.h>

/* Volatile keeps the calls reachable so --gc-sections cannot turn this into
 * a partial-API probe. The branch is deliberately false at runtime. */
static volatile pj_bool_t invoke_probe_calls;

int phase3_link_probe_run(void)
{
	pjmedia_sdp_neg *neg = NULL;
	pjmedia_sdp_session *session = NULL;
	pjmedia_sdp_media *media = NULL;
	const pjmedia_sdp_session *result = NULL;
	pj_pool_t *pool = NULL;
	pj_str_t fmt = { NULL, 0 };

	if (invoke_probe_calls) {
		(void)pjmedia_sdp_neg_state_str(PJMEDIA_SDP_NEG_STATE_NULL);
		(void)pjmedia_sdp_neg_create_w_local_offer(pool, session, &neg);
		(void)pjmedia_sdp_neg_create_w_remote_offer(pool, session, session,
						      &neg);
		(void)pjmedia_sdp_neg_set_prefer_remote_codec_order(neg, PJ_FALSE);
		(void)pjmedia_sdp_neg_set_answer_multiple_codecs(neg, PJ_FALSE);
		(void)pjmedia_sdp_neg_get_state(neg);
		(void)pjmedia_sdp_neg_get_active_local(neg, &result);
		(void)pjmedia_sdp_neg_get_active_remote(neg, &result);
		(void)pjmedia_sdp_neg_was_answer_remote(neg);
		(void)pjmedia_sdp_neg_get_neg_remote(neg, &result);
		(void)pjmedia_sdp_neg_get_neg_local(neg, &result);
		(void)pjmedia_sdp_neg_modify_local_offer(pool, neg, session);
		(void)pjmedia_sdp_neg_modify_local_offer2(pool, neg, 0, session);
		(void)pjmedia_sdp_neg_send_local_offer(pool, neg, &result);
		(void)pjmedia_sdp_neg_set_remote_answer(pool, neg, session);
		(void)pjmedia_sdp_neg_set_remote_offer(pool, neg, session);
		(void)pjmedia_sdp_neg_set_local_answer(pool, neg, session);
		(void)pjmedia_sdp_neg_has_local_answer(neg);
		(void)pjmedia_sdp_neg_cancel_offer(neg);
		(void)pjmedia_sdp_neg_negotiate(pool, neg, PJ_FALSE);
		(void)pjmedia_sdp_neg_register_fmt_match_cb(&fmt, NULL);
		(void)pjmedia_sdp_neg_fmt_match(pool, media, 0, media, 0, 0);
	}

	printk("PHASE 3 LINK PROBE: PASSED (all public negotiator APIs retained)\n");
	return 0;
}
