#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjsip.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_timer.h>

#include <zephyr/sys/printk.h>

#define PHASE4_LIFECYCLES 3
#define PHASE4_PARTIAL_STAGES 5
#define PHASE4_DRAIN_ATTEMPTS 32

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

static pj_status_t parse_test_sdp(pj_pool_t *pool,
					pjmedia_sdp_session **session)
{
	static const char offer[] =
		"v=0\r\n"
		"o=- 1 1 IN IP4 127.0.0.1\r\n"
		"s=phase4\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-16\r\n";
	pj_size_t length = sizeof(offer) - 1;
	char *copy = pj_pool_alloc(pool, length + 1);

	if (copy == NULL)
		return PJ_ENOMEM;
	pj_memcpy(copy, offer, length + 1);
	return pjmedia_sdp_parse(pool, copy, length, session);
}

static int drain_transactions(pjsip_endpoint *endpoint, const char *test)
{
	pj_time_val timeout = {0, 1};
	pj_status_t status;
	unsigned attempt;

	for (attempt = 0; attempt < PHASE4_DRAIN_ATTEMPTS; ++attempt) {
		if (pjsip_tsx_layer_get_tsx_count() == 0)
			return 0;
		status = pjsip_endpt_handle_events(endpoint, &timeout);
		if (status != PJ_SUCCESS)
			return fail_status(test, __LINE__, status);
	}

	return fail_value(test, __LINE__,
			  "pjsip_tsx_layer_get_tsx_count() == 0");
}

static int release_tdata(const char *test, pjsip_tx_data **tdata)
{
	pj_status_t status;

	if (*tdata == NULL)
		return 0;
	status = pjsip_tx_data_dec_ref(*tdata);
	*tdata = NULL;
	if (status != PJSIP_EBUFDESTROYED)
		return fail_status(test, __LINE__, status);
	return 0;
}

static int test_uac_session(const pjmedia_sdp_session *local_sdp)
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

	status = pjsip_inv_create_uac(dialog, local_sdp, 0, &invite);
	if (status != PJ_SUCCESS) {
		pj_status_t terminate_status = pjsip_dlg_terminate(dialog);

		if (terminate_status != PJ_SUCCESS)
			fail_status("failed UAC dialog cleanup", __LINE__,
				    terminate_status);
		return fail_status("pjsip_inv_create_uac", __LINE__, status);
	}

	status = pjsip_inv_terminate(invite, 500, PJ_FALSE);
	if (status != PJ_SUCCESS && status != PJ_EGONE)
		return fail_status("pjsip_inv_terminate", __LINE__, status);

	return 0;
}

static int test_uas_session_without_send(
	pjsip_endpoint *endpoint, const pjmedia_sdp_session *local_sdp)
{
	pjsip_tx_data *request = NULL;
	pjsip_rx_data rdata;
	pjsip_transport synthetic_transport;
	pjsip_dialog *dialog = NULL;
	pjsip_inv_session *invite = NULL;
	pjsip_transaction *transaction = NULL;
	pjsip_via_hdr *via;
	pj_str_t target = pj_str("sip:bob@example.net");
	pj_str_t from = pj_str("Alice <sip:alice@example.net>");
	pj_str_t to = pj_str("Bob <sip:bob@example.net>");
	pj_str_t contact = pj_str("sip:alice@example.net");
	pj_str_t local_contact = pj_str("sip:bob@example.net");
	pj_str_t received = pj_str("127.0.0.1");
	pj_status_t status;
	int result = -1;

	status = pjsip_endpt_create_request(endpoint, &pjsip_invite_method,
					    &target, &from, &to, &contact,
					    NULL, 1, NULL, &request);
	if (status != PJ_SUCCESS)
		return fail_status("in-memory INVITE creation", __LINE__, status);

	via = (pjsip_via_hdr *)pjsip_msg_find_hdr(request->msg, PJSIP_H_VIA,
						     NULL);
	if (via == NULL) {
		fail_value("in-memory INVITE Via", __LINE__, "via != NULL");
		goto release_request;
	}
	via->transport = pj_str("UDP");
	via->sent_by.host = received;
	via->sent_by.port = 5060;
	via->branch_param = pj_str(PJSIP_RFC3261_BRANCH_ID "-phase4");
	via->recvd_param = received;
	via->rport_param = -1;

	pj_bzero(&synthetic_transport, sizeof(synthetic_transport));
	synthetic_transport.key.type = PJSIP_TRANSPORT_UDP;
	synthetic_transport.flag = PJSIP_TRANSPORT_DATAGRAM;
	synthetic_transport.dir = PJSIP_TP_DIR_NONE;

	pj_bzero(&rdata, sizeof(rdata));
	rdata.tp_info.pool = request->pool;
	rdata.tp_info.transport = &synthetic_transport;
	rdata.msg_info.msg = request->msg;
	rdata.msg_info.from = (pjsip_from_hdr *)pjsip_msg_find_hdr(
		request->msg, PJSIP_H_FROM, NULL);
	rdata.msg_info.to = (pjsip_to_hdr *)pjsip_msg_find_hdr(
		request->msg, PJSIP_H_TO, NULL);
	rdata.msg_info.cid = (pjsip_cid_hdr *)pjsip_msg_find_hdr(
		request->msg, PJSIP_H_CALL_ID, NULL);
	rdata.msg_info.cseq = (pjsip_cseq_hdr *)pjsip_msg_find_hdr(
		request->msg, PJSIP_H_CSEQ, NULL);
	rdata.msg_info.via = via;
	if (rdata.msg_info.from == NULL || rdata.msg_info.to == NULL ||
	    rdata.msg_info.cid == NULL || rdata.msg_info.cseq == NULL) {
		fail_value("in-memory INVITE headers", __LINE__,
			   "From/To/Call-ID/CSeq present");
		goto release_request;
	}

	status = pjsip_dlg_create_uas_and_inc_lock(
		pjsip_ua_instance(), &rdata, &local_contact, &dialog);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_dlg_create_uas_and_inc_lock", __LINE__, status);
		goto release_request;
	}
	status = pjsip_inv_create_uas(dialog, &rdata, local_sdp, 0, &invite);
	pjsip_dlg_dec_lock(dialog);
	transaction = pjsip_rdata_get_tsx(&rdata);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_inv_create_uas", __LINE__, status);
		goto drain;
	}
	dialog = NULL;

	status = pjsip_inv_terminate(invite, 500, PJ_FALSE);
	if (status != PJ_SUCCESS) {
		fail_status("UAS pjsip_inv_terminate", __LINE__, status);
		goto drain;
	}
	invite = NULL;

	result = 0;
drain:
	if (transaction != NULL &&
	    transaction->state < PJSIP_TSX_STATE_TERMINATED)
		pjsip_tsx_terminate(transaction, 500);
	if (drain_transactions(endpoint, "UAS transaction drain") != 0)
		result = -1;
	if (pjsip_ua_get_dlg_set_count() != 0) {
		fail_value("UAS dialog cleanup", __LINE__,
			   "pjsip_ua_get_dlg_set_count() == 0");
		result = -1;
	}
release_request:
	if (release_tdata("in-memory INVITE release", &request) != 0)
		result = -1;
	return result;
}

static int test_partial_initialization_cleanup(unsigned stop_stage)
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
	if (status != PJ_SUCCESS) {
		fail_status("partial pjsip_tsx_layer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	if (stop_stage == 1) {
		result = 0;
		goto destroy_endpoint;
	}
	status = pjsip_ua_init_module(endpoint, &ua_param);
	if (status != PJ_SUCCESS) {
		fail_status("partial pjsip_ua_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	if (stop_stage == 2) {
		result = 0;
		goto destroy_endpoint;
	}
	status = pjsip_100rel_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("partial pjsip_100rel_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	if (stop_stage == 3) {
		result = 0;
		goto destroy_endpoint;
	}
	status = pjsip_timer_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("partial pjsip_timer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	if (stop_stage == 4) {
		result = 0;
		goto destroy_endpoint;
	}
	status = pjsip_inv_usage_init(endpoint, &inv_cb);
	if (status != PJ_SUCCESS) {
		fail_status("partial pjsip_inv_usage_init", __LINE__, status);
		goto destroy_endpoint;
	}
	if (stop_stage == 5) {
		result = 0;
		goto destroy_endpoint;
	}

destroy_endpoint:
	pjsip_endpt_destroy(endpoint);
	endpoint = NULL;
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("failure endpoint pool cleanup", __LINE__,
			   "caching_pool.used_count == 0 && caching_pool.capacity == 0");
		result = -1;
	}
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
	pjmedia_sdp_session *local_sdp = NULL;
	pj_pool_t *session_pool = NULL;
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
	session_pool = pjsip_endpt_create_pool(endpoint, "phase4-session", 4096,
					       4096);
	if (session_pool == NULL) {
		fail_value("session pool", __LINE__, "session_pool != NULL");
		goto destroy_endpoint;
	}
	status = parse_test_sdp(session_pool, &local_sdp);
	if (status != PJ_SUCCESS) {
		fail_status("session SDP parse", __LINE__, status);
		goto release_session_pool;
	}
	if (test_uac_session(local_sdp) != 0)
		goto release_session_pool;
	if (pjsip_ua_get_dlg_set_count() != 0) {
		fail_value("UAC dialog cleanup", __LINE__,
			   "pjsip_ua_get_dlg_set_count() == 0");
		goto release_session_pool;
	}
	if (test_uas_session_without_send(endpoint, local_sdp) != 0)
		goto release_session_pool;
	if (pjsip_tsx_layer_get_tsx_count() != 0 ||
	    pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)) != 0) {
		fail_value("session resource baseline", __LINE__,
			   "zero transactions and timers");
		goto release_session_pool;
	}
	result = 0;

release_session_pool:
	pjsip_endpt_release_pool(endpoint, session_pool);
	session_pool = NULL;
	if (result != 0)
		goto destroy_endpoint;

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
	for (failure_stage = 1; failure_stage <= PHASE4_PARTIAL_STAGES;
	     ++failure_stage) {
		if (test_partial_initialization_cleanup(failure_stage) != 0) {
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
