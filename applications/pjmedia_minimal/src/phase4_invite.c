#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjsip.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_timer.h>
#include <pjsip/sip_transport_loop.h>

#include <zephyr/sys/printk.h>

#define PHASE4_LIFECYCLES 3
#define PHASE4_FAILURE_STAGES 5

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 4] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 4] FAIL %s:%d condition=%s\n", test, line, condition);
	return -1;
}

static void on_state_changed(pjsip_inv_session *inv, pjsip_event *event)
{
	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(event);
}

struct uas_test_context {
	pjsip_endpoint *endpoint;
	pjsip_transport *loop;
	pjsip_inv_session *uas;
	pj_status_t callback_status;
	pj_bool_t received;
};

static struct uas_test_context *active_uas_test;

static pj_status_t parse_test_sdp(pj_pool_t *pool,
					pjmedia_sdp_session **session)
{
	static const char offer[] =
		"v=0\r\n"
		"o=- 1 1 IN IP4 127.0.0.1\r\n"
		"s=phase4\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0\r\n"
		"a=rtpmap:0 PCMU/8000\r\n";
	pj_size_t length = sizeof(offer) - 1;
	char *copy = pj_pool_alloc(pool, length + 1);

	if (copy == NULL)
		return PJ_ENOMEM;
	pj_memcpy(copy, offer, length + 1);
	return pjmedia_sdp_parse(pool, copy, length, session);
}

static pj_bool_t phase4_on_rx_request(pjsip_rx_data *rdata)
{
	struct uas_test_context *context = active_uas_test;
	pjsip_dialog *dialog = NULL;
	pjmedia_sdp_session *local_sdp = NULL;
	pj_str_t contact = pj_str("sip:bob@127.0.0.1;transport=loop-dgram");
	pj_status_t status;

	if (context == NULL || rdata->msg_info.msg == NULL ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_INVITE_METHOD)
		return PJ_FALSE;

	status = parse_test_sdp(rdata->tp_info.pool, &local_sdp);
	if (status == PJ_SUCCESS)
		status = pjsip_dlg_create_uas_and_inc_lock(
			pjsip_ua_instance(), rdata, &contact, &dialog);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_create_uas(dialog, rdata, local_sdp, 0,
					      &context->uas);
	if (dialog != NULL)
		pjsip_dlg_dec_lock(dialog);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_terminate(context->uas, 500, PJ_FALSE);

	context->callback_status = status;
	context->received = PJ_TRUE;
	return PJ_TRUE;
}

static pjsip_module phase4_uas_module = {
	.name = {"phase4-uas-validation", 22},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_UA_PROXY_LAYER - 1,
	.on_rx_request = &phase4_on_rx_request,
};

static int test_uas_loop_session(pjsip_endpoint *endpoint)
{
	struct uas_test_context context;
	pjsip_dialog *dialog = NULL;
	pjsip_inv_session *uac = NULL;
	pjsip_transport *loop = NULL;
	pjmedia_sdp_session *local_sdp = NULL;
	pj_pool_t *pool = NULL;
	pjsip_tx_data *invite_tdata = NULL;
	pj_str_t local_uri = pj_str("sip:alice@127.0.0.1;transport=loop-dgram");
	pj_str_t remote_uri = pj_str("sip:bob@127.0.0.1;transport=loop-dgram");
	pj_time_val timeout = {0, 10};
	pj_time_val deadline;
	pj_status_t status;
	int result = -1;

	pj_bzero(&context, sizeof(context));
	context.endpoint = endpoint;
	context.callback_status = PJ_EUNKNOWN;
	active_uas_test = &context;

	status = pjsip_endpt_register_module(endpoint, &phase4_uas_module);
	if (status != PJ_SUCCESS) {
		fail_status("UAS module registration", __LINE__, status);
		goto cleanup;
	}

	status = pjsip_loop_start(endpoint, &loop);
	if (status != PJ_SUCCESS) {
		fail_status("UAS loop start", __LINE__, status);
		goto unregister;
	}
	context.loop = loop;
	pool = pjsip_endpt_create_pool(endpoint, "phase4-uas", 4096, 4096);
	if (pool == NULL) {
		fail_value("UAS test pool", __LINE__, "pool != NULL");
		goto shutdown_loop;
	}
	status = parse_test_sdp(pool, &local_sdp);
	if (status != PJ_SUCCESS) {
		fail_status("UAC SDP parse", __LINE__, status);
		goto shutdown_loop;
	}
	status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local_uri, NULL,
				      &remote_uri, NULL, &dialog);
	if (status != PJ_SUCCESS) {
		fail_status("loop UAC dialog", __LINE__, status);
		goto shutdown_loop;
	}
	status = pjsip_inv_create_uac(dialog, local_sdp, 0, &uac);
	if (status != PJ_SUCCESS) {
		fail_status("loop UAC INVITE", __LINE__, status);
		pjsip_dlg_terminate(dialog);
		dialog = NULL;
		goto shutdown_loop;
	}
	status = pjsip_inv_invite(uac, &invite_tdata);
	if (status != PJ_SUCCESS) {
		fail_status("loop INVITE creation", __LINE__, status);
		goto terminate_uac;
	}
	status = pjsip_inv_send_msg(uac, invite_tdata);
	if (status != PJ_SUCCESS) {
		fail_status("loop INVITE send", __LINE__, status);
		goto terminate_uac;
	}

	pj_gettimeofday(&deadline);
	deadline.msec += 500;
	pj_time_val_normalize(&deadline);
	while (!context.received) {
		status = pjsip_endpt_handle_events(endpoint, &timeout);
		if (status != PJ_SUCCESS)
			break;
		{
			pj_time_val now;
			pj_gettimeofday(&now);
			if (PJ_TIME_VAL_GTE(now, deadline))
				break;
		}
	}
	if (!context.received || context.callback_status != PJ_SUCCESS) {
		if (context.received)
			fail_status("loop UAS session", __LINE__, context.callback_status);
		else
			fail_value("loop UAS session", __LINE__, "UAS callback received");
		goto terminate_uac;
	}
	result = 0;

terminate_uac:
	if (uac != NULL)
		pjsip_inv_terminate(uac, 500, PJ_FALSE);
	dialog = NULL;
shutdown_loop:
	if (pool != NULL)
		pjsip_endpt_release_pool(endpoint, pool);
	if (loop != NULL)
		pjsip_transport_shutdown(loop);
unregister:
	pjsip_endpt_unregister_module(endpoint, &phase4_uas_module);
cleanup:
	active_uas_test = NULL;
	return result;
}

static int test_uac_session(void)
{
	pjsip_dialog *dialog = NULL;
	pjsip_inv_session *invite = NULL;
	pj_str_t local_uri = pj_str("sip:alice@example.net");
	pj_str_t remote_uri = pj_str("sip:bob@example.net");
	pj_status_t status;

	status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local_uri, NULL,
				      &remote_uri, NULL, &dialog);
	if (status != PJ_SUCCESS)
		return fail_status("pjsip_dlg_create_uac", __LINE__, status);

	status = pjsip_inv_create_uac(dialog, NULL, 0, &invite);
	if (status != PJ_SUCCESS) {
		pjsip_dlg_terminate(dialog);
		return fail_status("pjsip_inv_create_uac", __LINE__, status);
	}

	status = pjsip_inv_terminate(invite, 500, PJ_FALSE);
	if (status != PJ_SUCCESS && status != PJ_EGONE)
		return fail_status("pjsip_inv_terminate", __LINE__, status);

	return 0;
}

static int test_initialization_cleanup(unsigned stop_stage)
{
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pjsip_ua_init_param ua_param;
	pjsip_inv_callback inv_cb;
	pj_status_t status;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("failure pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status("failure pjlib_util_init", __LINE__, status);
		goto shutdown;
	}
	pj_caching_pool_init(&caching_pool, NULL, 0);
	status = pjsip_endpt_create(&caching_pool.factory, "phase4-failure",
					&endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("failure pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}

	pj_bzero(&ua_param, sizeof(ua_param));
	pj_bzero(&inv_cb, sizeof(inv_cb));
	inv_cb.on_state_changed = &on_state_changed;
	status = pjsip_tsx_layer_init_module(endpoint);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	if (stop_stage == 1)
		goto destroy_endpoint;
	status = pjsip_ua_init_module(endpoint, &ua_param);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	if (stop_stage == 2)
		goto destroy_endpoint;
	status = pjsip_100rel_init_module(endpoint);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	if (stop_stage == 3)
		goto destroy_endpoint;
	status = pjsip_timer_init_module(endpoint);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	if (stop_stage == 4)
		goto destroy_endpoint;
	status = pjsip_inv_usage_init(endpoint, &inv_cb);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	if (stop_stage == 5)
		goto destroy_endpoint;

destroy_endpoint:
	pjsip_endpt_destroy(endpoint);
	endpoint = NULL;
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("failure endpoint pool cleanup", __LINE__,
			   "caching_pool.used_count == 0 && caching_pool.capacity == 0");
		goto destroy_factory;
	}
	result = 0;
destroy_factory:
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	if (result == 0)
		printk("[Phase 4] initialization stop %u cleanup: PASSED\n",
		       stop_stage);
	return result;
}

static int run_lifecycle(int iteration)
{
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pjsip_ua_init_param ua_param;
	pjsip_inv_callback inv_cb;
	pj_status_t status;
	int result = -1;

	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);

	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status("pjlib_util_init", __LINE__, status);
		goto shutdown;
	}

	pj_caching_pool_init(&caching_pool, NULL, 0);
	status = pjsip_endpt_create(&caching_pool.factory, "phase4", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}

	pj_bzero(&ua_param, sizeof(ua_param));
	pj_bzero(&inv_cb, sizeof(inv_cb));
	inv_cb.on_state_changed = &on_state_changed;
	if (inv_cb.on_state_changed == NULL) {
		fail_value("INVITE callback", __LINE__,
			   "inv_cb.on_state_changed != NULL");
		goto destroy_endpoint;
	}

	status = pjsip_tsx_layer_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tsx_layer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_ua_init_module(endpoint, &ua_param);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_ua_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_100rel_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_100rel_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_timer_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_timer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_inv_usage_init(endpoint, &inv_cb);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_inv_usage_init", __LINE__, status);
		goto destroy_endpoint;
	}
	if (test_uac_session() != 0)
		goto destroy_endpoint;
	if (test_uas_loop_session(endpoint) != 0)
		goto destroy_endpoint;
	result = 0;

destroy_endpoint:
	pjsip_endpt_destroy(endpoint);
	endpoint = NULL;
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("endpoint pool cleanup", __LINE__,
			   "caching_pool.used_count == 0 && caching_pool.capacity == 0");
		result = -1;
	}

destroy_factory:
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	if (result == 0)
		printk("[Phase 4] lifecycle %d teardown: PASSED\n", iteration);
	return result;
}

int phase4_invite_run(void)
{
	unsigned iteration;
	unsigned failure_stage;

	printk("[Phase 4] INVITE module validation (%d lifecycles)\n",
	       PHASE4_LIFECYCLES);
	for (failure_stage = 1; failure_stage <= PHASE4_FAILURE_STAGES;
	     ++failure_stage) {
		if (test_initialization_cleanup(failure_stage) != 0) {
			printk("PHASE 4 RESULT: FAILED at initialization stop %u\n",
			       failure_stage);
			return 1;
		}
	}
	for (iteration = 1; iteration <= PHASE4_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 4 RESULT: FAILED at lifecycle %u\n", iteration);
			return 1;
		}
	}

	printk("PHASE 4 RESULT: PASSED (%d complete INVITE module lifecycles)\n",
	       PHASE4_LIFECYCLES);
	return 0;
}
