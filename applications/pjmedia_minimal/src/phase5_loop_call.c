#include <pjsip.h>
#include <pjsip/sip_transport_loop.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_timer.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE5_LIFECYCLES 3
#define PHASE5_WAIT_MS 2200
#define PHASE5_CALL_COUNT 10

#define INV_STATE_BIT(state) ((atomic_val_t)1 << (state))

enum phase5_scenario {
	PHASE5_UAC_BYE,
	PHASE5_UAS_BYE,
	PHASE5_CANCEL,
	PHASE5_REJECT_4XX,
	PHASE5_REJECT_6XX,
	PHASE5_DISCARD_TIMEOUT,
	PHASE5_FAILURE_IMMEDIATE,
	PHASE5_FAILURE_DELAYED,
	PHASE5_OFFERLESS,
	PHASE5_REINVITE,
};

struct phase5_call {
	enum phase5_scenario scenario;
	pjsip_inv_session *uac;
	pjsip_inv_session *uas;

	atomic_t uac_states;
	atomic_t uas_states;
	atomic_t media_updates_uac;
	atomic_t media_updates_uas;
	atomic_t media_error;
	atomic_t rx_reinvite;

	atomic_t request_invite;
	atomic_t request_ack;
	atomic_t request_cancel;
	atomic_t request_bye;
	atomic_t response_100;
	atomic_t response_180;
	atomic_t response_200_invite;
	atomic_t wire_200_invite_count;
	atomic_t response_200_cancel;
	atomic_t response_200_bye;
	atomic_t response_486;
	atomic_t response_487;
	atomic_t response_488;
	atomic_t response_500;
	atomic_t response_603;
};

struct phase5_context {
	pjsip_endpoint *endpt;
	pjsip_transport *loop;
	pj_thread_t *event_thread;
	struct phase5_call *call;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t timer_fired;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;
	pj_timer_entry response_timer;
	pjsip_inv_session *response_inv;
	int response_stage;
	int response_final_code;
};

static struct phase5_context *active_context;
static pjsip_module phase5_module;
static pjsip_module phase5_wire_module;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 5] FAIL %s:%d status=%d (%s)\n", test, line, status,
	       text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 5] FAIL %s:%d condition=%s\n", test, line, condition);
	return -1;
}

#define CHECK_STATUS(test_name, expression)                                  \
	do {                                                                     \
		pj_status_t check_status_ = (expression);                           \
		if (check_status_ != PJ_SUCCESS)                                    \
			return fail_status((test_name), __LINE__, check_status_);      \
	} while (0)

#define CHECK_TRUE(test_name, condition)                                     \
	do {                                                                     \
		if (!(condition))                                                  \
			return fail_value((test_name), __LINE__, #condition);         \
	} while (0)

static void record_callback_error(int error)
{
	if (active_context != NULL)
		atomic_cas(&active_context->callback_error, 0, error);
}

static void make_deadline(pj_time_val *deadline, unsigned timeout_ms)
{
	pj_gettimeofday(deadline);
	deadline->msec += timeout_ms;
	pj_time_val_normalize(deadline);
}

static pj_bool_t deadline_reached(const pj_time_val *deadline)
{
	pj_time_val now;

	pj_gettimeofday(&now);
	return PJ_TIME_VAL_GTE(now, *deadline);
}

static int wait_for_value(atomic_t *value, atomic_val_t minimum,
			  unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (atomic_get(value) < minimum) {
		if (active_context == NULL ||
		    atomic_get(&active_context->event_error) != 0 ||
		    atomic_get(&active_context->callback_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_bits(atomic_t *value, atomic_val_t expected,
			 unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while ((atomic_get(value) & expected) != expected) {
		if (active_context == NULL ||
		    atomic_get(&active_context->event_error) != 0 ||
		    atomic_get(&active_context->callback_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_quiescence(struct phase5_context *context,
			       unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (pjsip_tsx_layer_get_tsx_count() != 0 ||
	       pjsip_ua_get_dlg_set_count() != 0 ||
	       pj_timer_heap_count(pjsip_endpt_get_timer_heap(context->endpt)) !=
		       0) {
		if (atomic_get(&context->event_error) != 0 ||
		    atomic_get(&context->callback_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static const char *scenario_name(enum phase5_scenario scenario)
{
	static const char *const names[PHASE5_CALL_COUNT] = {
		"UAC BYE", "UAS BYE", "CANCEL/487", "486 rejection",
		"603 rejection", "discard/timeout", "immediate failure",
		"delayed failure", "offerless INVITE", "re-INVITE/hold",
	};

	return names[scenario];
}

static pj_status_t parse_sdp(pj_pool_t *pool, const char *direction,
			     pj_bool_t compatible,
			     pjmedia_sdp_session **session)
{
	static const char sendrecv[] =
		"v=0\r\n"
		"o=phase5 1 1 IN IP4 127.0.0.1\r\n"
		"s=phase5-loop-call\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=sendrecv\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-16\r\n";
	static const char sendonly[] =
		"v=0\r\n"
		"o=phase5 2 2 IN IP4 127.0.0.1\r\n"
		"s=phase5-loop-call\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=sendonly\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-16\r\n";
	static const char recvonly[] =
		"v=0\r\n"
		"o=phase5 2 2 IN IP4 127.0.0.1\r\n"
		"s=phase5-loop-call\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 0 8 101\r\n"
		"a=recvonly\r\n"
		"a=rtpmap:0 PCMU/8000\r\n"
		"a=rtpmap:8 PCMA/8000\r\n"
		"a=rtpmap:101 telephone-event/8000\r\n"
		"a=fmtp:101 0-16\r\n";
	static const char incompatible[] =
		"v=0\r\n"
		"o=phase5 3 3 IN IP4 127.0.0.1\r\n"
		"s=phase5-loop-call\r\n"
		"c=IN IP4 127.0.0.1\r\n"
		"t=0 0\r\n"
		"m=audio 4000 RTP/AVP 18\r\n"
		"a=sendrecv\r\n"
		"a=rtpmap:18 G729/8000\r\n";
	const char *text;
	pj_size_t length;
	char *copy;

	if (!compatible)
		text = incompatible;
	else if (pj_ansi_strcmp(direction, "sendonly") == 0)
		text = sendonly;
	else if (pj_ansi_strcmp(direction, "recvonly") == 0)
		text = recvonly;
	else
		text = sendrecv;
	length = pj_ansi_strlen(text);
	copy = pj_pool_alloc(pool, length + 1);
	if (copy == NULL)
		return PJ_ENOMEM;
	pj_memcpy(copy, text, length + 1);
	return pjmedia_sdp_parse(pool, copy, length, session);
}

static struct phase5_call *current_call(void)
{
	if (active_context == NULL || active_context->call == NULL) {
		record_callback_error(-100);
		return NULL;
	}
	return active_context->call;
}

static void on_state_changed(pjsip_inv_session *inv, pjsip_event *event)
{
	struct phase5_call *call = current_call();
	atomic_t *states;

	PJ_UNUSED_ARG(event);
	if (call == NULL)
		return;
	states = inv->role == PJSIP_ROLE_UAC ? &call->uac_states :
						  &call->uas_states;
	atomic_or(states, INV_STATE_BIT(inv->state));
	printk("[Phase 5] %s %s -> %s\n", scenario_name(call->scenario),
	       inv->role == PJSIP_ROLE_UAC ? "UAC" : "UAS",
	       pjsip_inv_state_name(inv->state));
}

static void on_media_update(pjsip_inv_session *inv, pj_status_t status)
{
	struct phase5_call *call = current_call();

	if (call == NULL)
		return;
	if (status != PJ_SUCCESS) {
		atomic_set(&call->media_error, status);
		return;
	}
	if (inv->role == PJSIP_ROLE_UAC)
		atomic_inc(&call->media_updates_uac);
	else
		atomic_inc(&call->media_updates_uas);
}

static void on_rx_offer(pjsip_inv_session *inv,
			const pjmedia_sdp_session *offer)
{
	struct phase5_call *call = current_call();
	pjmedia_sdp_session *answer = NULL;
	const char *direction = "sendrecv";
	pj_status_t status;

	PJ_UNUSED_ARG(offer);
	if (call == NULL)
		return;
	if (call->scenario == PHASE5_REINVITE &&
	    atomic_get(&call->request_invite) >= 2)
		direction = "recvonly";
	status = parse_sdp(inv->pool_prov, direction, PJ_TRUE, &answer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_set_sdp_answer(inv, answer);
	if (status != PJ_SUCCESS)
		record_callback_error(status);
}

static pj_status_t on_rx_reinvite(pjsip_inv_session *inv,
				  const pjmedia_sdp_session *offer,
				  pjsip_rx_data *rdata)
{
	struct phase5_call *call = current_call();

	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(offer);
	PJ_UNUSED_ARG(rdata);
	if (call == NULL)
		return PJ_EINVALIDOP;
	atomic_inc(&call->rx_reinvite);
	return PJ_EIGNORED;
}

static void count_response(struct phase5_call *call, int code,
			   pjsip_method_e method)
{
	if (method == PJSIP_INVITE_METHOD) {
		if (code == 100)
			atomic_set(&call->response_100, 1);
		else if (code == 180)
			atomic_set(&call->response_180, 1);
		else if (code == 200)
			atomic_set(&call->response_200_invite, 1);
		else if (code == 486)
			atomic_set(&call->response_486, 1);
		else if (code == 487)
			atomic_set(&call->response_487, 1);
		else if (code == 488)
			atomic_set(&call->response_488, 1);
		else if (code == 500)
			atomic_set(&call->response_500, 1);
		else if (code == 603)
			atomic_set(&call->response_603, 1);
	} else if (method == PJSIP_CANCEL_METHOD && code == 200) {
		atomic_set(&call->response_200_cancel, 1);
	} else if (method == PJSIP_BYE_METHOD && code == 200) {
		atomic_set(&call->response_200_bye, 1);
	}
}

static pj_bool_t phase5_on_rx_response(pjsip_rx_data *rdata)
{
	struct phase5_call *call = current_call();

	if (call != NULL && rdata->tp_info.transport == active_context->loop &&
	    rdata->msg_info.cseq != NULL)
		count_response(call, rdata->msg_info.msg->line.status.code,
			       rdata->msg_info.cseq->method.id);
	return PJ_FALSE;
}

static pj_status_t phase5_on_tx_response(pjsip_tx_data *tdata)
{
	struct phase5_call *call = current_call();
	pjsip_cseq_hdr *cseq;

	if (call == NULL)
		return PJ_SUCCESS;
	cseq = (pjsip_cseq_hdr *)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CSEQ,
						      NULL);
	if (cseq != NULL)
		count_response(call, tdata->msg->line.status.code,
			       cseq->method.id);
	if (cseq != NULL && cseq->method.id == PJSIP_INVITE_METHOD &&
	    tdata->msg->line.status.code == 200)
		atomic_inc(&call->wire_200_invite_count);
	printk("[Phase 5] wire TX response %d cseq-method=%d\n",
	       tdata->msg->line.status.code, cseq != NULL ? cseq->method.id : -1);
	return PJ_SUCCESS;
}

static pj_status_t phase5_on_tx_request(pjsip_tx_data *tdata)
{
	struct phase5_call *call = current_call();
	pjsip_method_e method;

	if (call == NULL)
		return PJ_SUCCESS;
	method = tdata->msg->line.req.method.id;
	if (method == PJSIP_INVITE_METHOD)
		atomic_inc(&call->request_invite);
	else if (method == PJSIP_ACK_METHOD)
		atomic_inc(&call->request_ack);
	else if (method == PJSIP_CANCEL_METHOD)
		atomic_inc(&call->request_cancel);
	else if (method == PJSIP_BYE_METHOD)
		atomic_inc(&call->request_bye);
	return PJ_SUCCESS;
}

static pj_status_t send_answer(pjsip_inv_session *inv, pjsip_rx_data *rdata,
			       int code, pj_bool_t initial)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	if (initial)
		status = pjsip_inv_initial_answer(inv, rdata, code, NULL, NULL,
						  &tdata);
	else
		status = pjsip_inv_answer(inv, code, NULL, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return status;
	status = pjsip_inv_send_msg(inv, tdata);
	if (status == PJ_SUCCESS && active_context != NULL &&
	    active_context->call != NULL)
		count_response(active_context->call, code, PJSIP_INVITE_METHOD);
	return status;
}

static void phase5_response_timer(pj_timer_heap_t *timer_heap,
				  pj_timer_entry *entry)
{
	struct phase5_context *context = entry->user_data;
	pj_time_val delay = {0, 20};
	pj_status_t status;

	PJ_UNUSED_ARG(timer_heap);
	if (context->response_stage == 180) {
		status = send_answer(context->response_inv, NULL, 180, PJ_FALSE);
		if (status == PJ_SUCCESS && context->response_final_code != 0) {
			context->response_stage = context->response_final_code;
			status = pjsip_endpt_schedule_timer(context->endpt,
							      entry, &delay);
		} else {
			context->response_stage = 0;
		}
	} else {
		status = send_answer(context->response_inv, NULL,
				     context->response_stage, PJ_FALSE);
		context->response_stage = 0;
	}
	if (status != PJ_SUCCESS)
		record_callback_error(status);
}

static pj_bool_t phase5_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase5_call *call = current_call();
	pjsip_dialog *dialog = NULL;
	pjsip_inv_session *invite = NULL;
	pjmedia_sdp_session *answer = NULL;
	pjsip_tpselector selector;
	pj_str_t contact = pj_str("sip:bob@127.0.0.1;transport=loop-dgram");
	pjsip_method_e method;
	pj_status_t status;
	int final_code = 200;

	if (call == NULL || rdata->tp_info.transport != active_context->loop)
		return PJ_FALSE;
	method = rdata->msg_info.msg->line.req.method.id;

	/* In-dialog requests are owned by the INVITE usage/dialog modules. */
	if (method != PJSIP_INVITE_METHOD || rdata->msg_info.to == NULL ||
	    rdata->msg_info.to->tag.slen != 0)
		return PJ_FALSE;

	status = pjsip_dlg_create_uas_and_inc_lock(
		pjsip_ua_instance(), rdata, &contact, &dialog);
	if (status != PJ_SUCCESS) {
		record_callback_error(status);
		return PJ_TRUE;
	}
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = active_context->loop;
	status = pjsip_dlg_set_transport(dialog, &selector);
	if (status == PJ_SUCCESS)
		status = parse_sdp(dialog->pool, "sendrecv", PJ_TRUE, &answer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_create_uas(dialog, rdata, answer, 0, &invite);
	if (status == PJ_SUCCESS) {
		call->uas = invite;
		dialog->mod_data[phase5_module.id] = call;
	}
	pjsip_dlg_dec_lock(dialog);
	if (status != PJ_SUCCESS) {
		record_callback_error(status);
		return PJ_TRUE;
	}

	status = send_answer(invite, rdata, 100, PJ_TRUE);
	if (status != PJ_SUCCESS) {
		record_callback_error(status);
		return PJ_TRUE;
	}
	if (call->scenario == PHASE5_REJECT_4XX)
		final_code = PJSIP_SC_BUSY_HERE;
	else if (call->scenario == PHASE5_REJECT_6XX)
		final_code = PJSIP_SC_DECLINE;
	active_context->response_inv = invite;
	active_context->response_stage = 180;
	active_context->response_final_code =
		call->scenario == PHASE5_CANCEL ? 0 : final_code;
	pj_timer_entry_init(&active_context->response_timer, 180, active_context,
			    phase5_response_timer);
	{
		pj_time_val delay = {0, 20};

		status = pjsip_endpt_schedule_timer(active_context->endpt,
						    &active_context->response_timer,
						    &delay);
	}
	if (status != PJ_SUCCESS)
		record_callback_error(status);
	return PJ_TRUE;
}

static pjsip_module phase5_module = {
	.name = {"phase5-loop-call", 16},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_rx_request = phase5_on_rx_request,
	.on_rx_response = phase5_on_rx_response,
};

static pjsip_module phase5_wire_module = {
	.name = {"phase5-wire-observer", 20},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_TRANSPORT_LAYER + 1,
	.on_tx_request = phase5_on_tx_request,
	.on_tx_response = phase5_on_tx_response,
};

static int phase5_event_thread(void *arg)
{
	struct phase5_context *context = arg;
	pj_time_val timeout = {0, 10};

	atomic_set(&context->event_started, 1);
	while (!atomic_get(&context->event_stop)) {
		pj_status_t status = pjsip_endpt_handle_events(context->endpt,
							    &timeout);

		atomic_inc(&context->event_polls);
		if (status != PJ_SUCCESS) {
			atomic_set(&context->event_error, status);
			break;
		}
	}
	return 0;
}

static void phase5_timer_callback(pj_timer_heap_t *timer_heap,
				  pj_timer_entry *entry)
{
	struct phase5_context *context = entry->user_data;

	PJ_UNUSED_ARG(timer_heap);
	atomic_set(&context->timer_fired, 1);
}

static void phase5_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static int test_idle_pump(struct phase5_context *context)
{
	const char *test = "loop transport idle event pump";
	pj_timer_entry timer;
	pj_time_val delay = {0, 25};

	pj_timer_entry_init(&timer, 1, context, phase5_timer_callback);
	CHECK_STATUS(test, pjsip_endpt_schedule_timer(context->endpt, &timer,
						     &delay));
	CHECK_TRUE(test, wait_for_value(&context->timer_fired, 1,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->event_polls) > 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE5_WAIT_MS) == 0);
	printk("[Phase 5] loop transport, event pump, and endpoint timer: PASSED\n");
	return 0;
}

static int start_call(struct phase5_context *context, struct phase5_call *call,
		      pj_bool_t with_offer, pj_bool_t tolerate_send_error)
{
	const char *test = scenario_name(call->scenario);
	pjsip_dialog *dialog = NULL;
	pjsip_tpselector selector;
	pjmedia_sdp_session *offer = NULL;
	pjsip_tx_data *tdata = NULL;
	pj_str_t local = pj_str("sip:alice@127.0.0.1;transport=loop-dgram");
	pj_str_t remote = pj_str("sip:bob@127.0.0.1;transport=loop-dgram");
	pj_status_t status;

	context->call = call;
	status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local, &local,
				      &remote, &remote, &dialog);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	/* Keep the zero-usage dialog alive across set_transport(). */
	pjsip_dlg_inc_lock(dialog);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->loop;
	status = pjsip_dlg_set_transport(dialog, &selector);
	if (status == PJ_SUCCESS && with_offer)
		status = parse_sdp(dialog->pool, "sendrecv", PJ_TRUE, &offer);
	if (status == PJ_SUCCESS && offer != NULL)
		status = pjmedia_sdp_validate(offer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_create_uac(dialog, offer, 0, &call->uac);
	if (status != PJ_SUCCESS) {
		pjsip_dlg_dec_lock(dialog);
		return fail_status(test, __LINE__, status);
	}
	dialog->mod_data[phase5_module.id] = call;
	pjsip_dlg_dec_lock(dialog);
	status = pjsip_inv_invite(call->uac, &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_send_msg(call->uac, tdata);
	if (status != PJ_SUCCESS && !tolerate_send_error)
		return fail_status(test, __LINE__, status);
	return 0;
}

static int finish_call(struct phase5_context *context,
		       struct phase5_call *call, pjsip_inv_session *initiator)
{
	const char *test = scenario_name(call->scenario);
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_inv_end_session(initiator, PJSIP_SC_OK, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	if (tdata != NULL) {
		status = pjsip_inv_send_msg(initiator, tdata);
		if (status != PJ_SUCCESS)
			return fail_status(test, __LINE__, status);
	}
	CHECK_TRUE(test, wait_for_bits(&call->uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call->uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call->request_bye) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_200_bye) == 1);
	context->call = NULL;
	return 0;
}

static int wait_confirmed(struct phase5_call *call)
{
	const char *test = scenario_name(call->scenario);
	atomic_val_t confirmed = INV_STATE_BIT(PJSIP_INV_STATE_EARLY) |
				 INV_STATE_BIT(PJSIP_INV_STATE_CONFIRMED);

	CHECK_TRUE(test, wait_for_bits(&call->uac_states, confirmed,
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call->uas_states, confirmed,
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call->request_invite) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_100) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_180) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_200_invite) == 1);
	CHECK_TRUE(test, atomic_get(&call->request_ack) == 1);
	CHECK_TRUE(test, atomic_get(&call->media_updates_uac) >= 1);
	CHECK_TRUE(test, atomic_get(&call->media_updates_uas) >= 1);
	CHECK_TRUE(test, atomic_get(&call->media_error) == 0);
	return 0;
}

static int test_connected_call(struct phase5_context *context,
			       enum phase5_scenario scenario)
{
	struct phase5_call call;

	pj_bzero(&call, sizeof(call));
	call.scenario = scenario;
	context->call = &call;
	CHECK_TRUE(scenario_name(scenario),
		   start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(scenario_name(scenario), wait_confirmed(&call) == 0);
	CHECK_TRUE(scenario_name(scenario),
		   finish_call(context, &call,
			       scenario == PHASE5_UAS_BYE ? call.uas : call.uac) == 0);
	printk("[Phase 5] %s call flow: PASSED\n", scenario_name(scenario));
	return 0;
}

static int test_cancel(struct phase5_context *context)
{
	const char *test = "CANCEL/487";
	struct phase5_call call;
	pjsip_tx_data *tdata = NULL;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE5_CANCEL;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_EARLY),
				      PHASE5_WAIT_MS) == 0);
	CHECK_STATUS(test, pjsip_inv_end_session(call.uac,
						PJSIP_SC_REQUEST_TERMINATED,
						NULL, &tdata));
	CHECK_TRUE(test, tdata != NULL);
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_cancel) == 1);
	CHECK_TRUE(test, atomic_get(&call.response_200_cancel) == 1);
	CHECK_TRUE(test, atomic_get(&call.response_487) == 1);
	context->call = NULL;
	printk("[Phase 5] CANCEL -> 200 and INVITE -> 487: PASSED\n");
	return 0;
}

static int test_rejection(struct phase5_context *context,
			  enum phase5_scenario scenario)
{
	const char *test = scenario_name(scenario);
	struct phase5_call call;
	atomic_t *response;

	pj_bzero(&call, sizeof(call));
	call.scenario = scenario;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE5_WAIT_MS) == 0);
	response = scenario == PHASE5_REJECT_4XX ? &call.response_486 :
						      &call.response_603;
	CHECK_TRUE(test, atomic_get(response) == 1);
	context->call = NULL;
	printk("[Phase 5] %s: PASSED\n", test);
	return 0;
}

static int test_transport_case(struct phase5_context *context,
			       enum phase5_scenario scenario)
{
	const char *test = scenario_name(scenario);
	struct phase5_call call;
	pj_bool_t discard = scenario == PHASE5_DISCARD_TIMEOUT;
	unsigned delay = scenario == PHASE5_FAILURE_IMMEDIATE ? 0 : 5;
	pj_status_t status;

	CHECK_STATUS(test, pjsip_loop_set_delay(context->loop, delay));
	if (discard)
		CHECK_STATUS(test, pjsip_loop_set_discard(context->loop, PJ_TRUE,
							   NULL));
	else
		CHECK_STATUS(test, pjsip_loop_set_failure(context->loop, 1, NULL));
	pj_bzero(&call, sizeof(call));
	call.scenario = scenario;
	context->call = &call;
	status = start_call(context, &call, PJ_TRUE, PJ_TRUE);
	if (status != 0)
		return status;
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE5_WAIT_MS) == 0);
	if (discard)
		CHECK_STATUS(test, pjsip_loop_set_discard(context->loop, PJ_FALSE,
							   NULL));
	else
		CHECK_STATUS(test, pjsip_loop_set_failure(context->loop, 0, NULL));
	CHECK_STATUS(test, pjsip_loop_set_delay(context->loop, 5));
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, call.uas == NULL);
	context->call = NULL;
	printk("[Phase 5] %s path and cleanup: PASSED\n", test);
	return 0;
}

static int test_offerless(struct phase5_context *context)
{
	const char *test = "offerless INVITE";
	struct phase5_call call;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE5_OFFERLESS;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_FALSE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_TRUE(test, finish_call(context, &call, call.uac) == 0);
	printk("[Phase 5] offerless INVITE with answer in ACK: PASSED\n");
	return 0;
}

static int test_reinvite(struct phase5_context *context)
{
	const char *test = "re-INVITE/hold";
	struct phase5_call call;
	pjmedia_sdp_session *offer = NULL;
	const pjmedia_sdp_session *active = NULL;
	pjsip_tx_data *tdata = NULL;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE5_REINVITE;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_STATUS(test, parse_sdp(call.uac->pool_prov, "sendonly", PJ_TRUE,
					    &offer));
	CHECK_STATUS(test, pjsip_inv_reinvite(call.uac, NULL, offer, &tdata));
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_value(&call.rx_reinvite, 1,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.wire_200_invite_count, 2,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.media_updates_uac, 2,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.media_updates_uas, 2,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.media_error) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_invite) == 2);
	CHECK_TRUE(test, atomic_get(&call.request_ack) == 2);
	CHECK_STATUS(test, pjmedia_sdp_neg_get_active_local(call.uac->neg,
							   &active));
	CHECK_TRUE(test, active->media_count == 1 &&
			 pjmedia_sdp_media_find_attr2(active->media[0], "sendonly",
						       NULL) != NULL);
	CHECK_STATUS(test, pjmedia_sdp_neg_get_active_remote(call.uac->neg,
							    &active));
	CHECK_TRUE(test, active->media_count == 1 &&
			 pjmedia_sdp_media_find_attr2(active->media[0], "recvonly",
						       NULL) != NULL);

	/* A no-common-codec re-INVITE must be rejected without ending the call. */
	offer = NULL;
	tdata = NULL;
	CHECK_STATUS(test, parse_sdp(call.uac->pool_prov, "sendrecv", PJ_FALSE,
					    &offer));
	CHECK_STATUS(test, pjsip_inv_reinvite(call.uac, NULL, offer, &tdata));
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_value(&call.response_500, 1,
				       PHASE5_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_invite) == 3);
	CHECK_TRUE(test, atomic_get(&call.uac_states) &
			 INV_STATE_BIT(PJSIP_INV_STATE_CONFIRMED));
	CHECK_TRUE(test, finish_call(context, &call, call.uac) == 0);
	printk("[Phase 5] re-INVITE hold and incompatible SDP rejection: PASSED\n");
	return 0;
}

static int run_lifecycle(int iteration)
{
	struct phase5_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pjsip_transport *loop = NULL;
	pj_pool_t *thread_pool = NULL;
	pjsip_ua_init_param ua_param;
	pjsip_inv_callback inv_cb;
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t wire_module_registered = PJ_FALSE;
	pj_bool_t loop_ref_held = PJ_FALSE;
	int result = -1;

	pj_bzero(&context, sizeof(context));
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	pj_log_set_level(3);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status("pjlib_util_init", __LINE__, status);
		goto shutdown;
	}
	pj_caching_pool_init(&caching_pool, NULL, 0);
	caching_pool_initialized = PJ_TRUE;
	status = pjsip_endpt_create(&caching_pool.factory, "phase5", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
	status = pjsip_endpt_atexit(endpoint, phase5_endpoint_exit);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_atexit", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_tsx_layer_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tsx_layer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	pj_bzero(&ua_param, sizeof(ua_param));
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
	pj_bzero(&inv_cb, sizeof(inv_cb));
	inv_cb.on_state_changed = on_state_changed;
	inv_cb.on_media_update = on_media_update;
	inv_cb.on_rx_offer = on_rx_offer;
	inv_cb.on_rx_reinvite = on_rx_reinvite;
	status = pjsip_inv_usage_init(endpoint, &inv_cb);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_inv_usage_init", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_endpt_register_module(endpoint, &phase5_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;
	status = pjsip_endpt_register_module(endpoint, &phase5_wire_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module wire observer", __LINE__,
			    status);
		goto destroy_endpoint;
	}
	wire_module_registered = PJ_TRUE;
	status = pjsip_loop_start(endpoint, &loop);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_loop_start", __LINE__, status);
		goto destroy_endpoint;
	}
	pjsip_transport_add_ref(loop);
	loop_ref_held = PJ_TRUE;
	context.loop = loop;
	if (loop->key.type != PJSIP_TRANSPORT_LOOP_DGRAM ||
	    !(loop->flag & PJSIP_TRANSPORT_DATAGRAM) || loop->endpt != endpoint ||
	    loop->tpmgr != pjsip_endpt_get_tpmgr(endpoint)) {
		fail_value("loop transport", __LINE__, "loop belongs to endpoint");
		goto destroy_endpoint;
	}
	status = pjsip_loop_set_delay(loop, 5);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_loop_set_delay", __LINE__, status);
		goto destroy_endpoint;
	}
	thread_pool = pjsip_endpt_create_pool(endpoint, "phase5-thread", 4096,
					      4096);
	if (thread_pool == NULL) {
		fail_value("event thread pool", __LINE__, "thread_pool != NULL");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p5-event", phase5_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("pj_thread_create", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_value(&context.event_started, 1, PHASE5_WAIT_MS) != 0) {
		fail_value("event pump", __LINE__, "event thread started");
		goto destroy_endpoint;
	}

	if (test_idle_pump(&context) != 0 ||
	    test_connected_call(&context, PHASE5_UAC_BYE) != 0 ||
	    test_connected_call(&context, PHASE5_UAS_BYE) != 0 ||
	    test_cancel(&context) != 0 ||
	    test_rejection(&context, PHASE5_REJECT_4XX) != 0 ||
	    test_rejection(&context, PHASE5_REJECT_6XX) != 0 ||
	    test_transport_case(&context, PHASE5_DISCARD_TIMEOUT) != 0 ||
	    test_transport_case(&context, PHASE5_FAILURE_IMMEDIATE) != 0 ||
	    test_transport_case(&context, PHASE5_FAILURE_DELAYED) != 0 ||
	    test_offerless(&context) != 0 || test_reinvite(&context) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.callback_error) != 0 ||
	    atomic_get(&context.event_error) != 0)
		goto destroy_endpoint;
	result = 0;

destroy_endpoint:
	context.call = NULL;
	if (loop_ref_held) {
		status = pjsip_loop_set_discard(loop, PJ_FALSE, NULL);
		if (status == PJ_SUCCESS)
			status = pjsip_loop_set_failure(loop, 0, NULL);
		if (status == PJ_SUCCESS)
			status = pjsip_transport_shutdown(loop);
		if (status != PJ_SUCCESS) {
			fail_status("loop transport shutdown", __LINE__, status);
			result = -1;
		}
		pjsip_transport_dec_ref(loop);
		loop_ref_held = PJ_FALSE;
		context.loop = NULL;
	}
	if (result == 0 && context.event_thread != NULL &&
	    wait_for_quiescence(&context, PHASE5_WAIT_MS) != 0) {
		fail_value("loop shutdown drain", __LINE__,
			   "zero transactions, dialogs, and timers");
		result = -1;
	}
	if (context.event_thread != NULL) {
		atomic_set(&context.event_stop, 1);
		status = pj_thread_join(context.event_thread);
		if (status == PJ_SUCCESS)
			status = pj_thread_destroy(context.event_thread);
		if (status != PJ_SUCCESS) {
			fail_status("event thread teardown", __LINE__, status);
			result = -1;
		}
		context.event_thread = NULL;
	}
	if (module_registered) {
		status = pjsip_endpt_unregister_module(endpoint, &phase5_module);
		if (status != PJ_SUCCESS) {
			fail_status("module unregister", __LINE__, status);
			result = -1;
		}
	}
	if (wire_module_registered) {
		status = pjsip_endpt_unregister_module(endpoint,
							 &phase5_wire_module);
		if (status != PJ_SUCCESS) {
			fail_status("wire module unregister", __LINE__, status);
			result = -1;
		}
	}
	if (thread_pool != NULL)
		pj_pool_release(thread_pool);
	if (result == 0 &&
	    (pjsip_tsx_layer_get_tsx_count() != 0 ||
	     pjsip_ua_get_dlg_set_count() != 0 ||
	     pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)) != 0)) {
		printk("[Phase 5] teardown counts: tsx=%u dialogs=%u timers=%u\n",
		       pjsip_tsx_layer_get_tsx_count(),
		       pjsip_ua_get_dlg_set_count(),
		       pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)));
		fail_value("lifecycle teardown", __LINE__,
			   "zero transactions, dialogs, and timers");
		result = -1;
	}
	pjsip_endpt_destroy(endpoint);
	if (atomic_get(&context.endpoint_exit_count) != 1) {
		fail_value("endpoint atexit", __LINE__, "one endpoint exit callback");
		result = -1;
	}
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("pool cleanup", __LINE__, "caching pool empty");
		result = -1;
	}
	if (result == 0)
		printk("[Phase 5] lifecycle %d complete teardown: PASSED\n",
		       iteration);
destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	return result;
}

int phase5_loop_call_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	int iteration;

	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 320;
	printk("[Phase 5] socket-free INVITE call control (%d lifecycles)\n",
	       PHASE5_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE5_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 5 RESULT: FAILED at lifecycle %d\n", iteration);
			pjsip_cfg()->tsx.t1 = saved_t1;
			pjsip_cfg()->tsx.t2 = saved_t2;
			pjsip_cfg()->tsx.t4 = saved_t4;
			pjsip_cfg()->tsx.td = saved_td;
			return 1;
		}
	}
	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	printk("PHASE 5 RESULT: PASSED (3 complete socket-free call lifecycles)\n");
	return 0;
}
