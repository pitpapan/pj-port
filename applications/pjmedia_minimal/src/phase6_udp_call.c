#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pjsip-ua/sip_100rel.h>
#include <pjsip-ua/sip_inv.h>
#include <pjsip-ua/sip_regc.h>
#include <pjsip-ua/sip_timer.h>
#include <pjmedia/sdp.h>
#include <pjmedia/sdp_neg.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
#include "phase11_media.h"
#endif

#define PHASE6_LIFECYCLES 3
#define PHASE6_WAIT_MS 2200
#define PHASE6_CALL_COUNT 12

#define INV_STATE_BIT(state) ((atomic_val_t)1 << (state))

enum phase6_scenario {
	PHASE6_UAC_BYE,
	PHASE6_UAS_BYE,
	PHASE6_CANCEL,
	PHASE6_REJECT_4XX,
	PHASE6_REJECT_6XX,
	PHASE6_DROP_RETRY,
	PHASE6_UDP_TIMEOUT,
	PHASE6_OFFERLESS,
	PHASE6_REINVITE,
	PHASE6_UAC_CLOSE_EARLY,
	PHASE6_UAS_CLOSE_CONFIRMED,
};

struct phase6_call {
	enum phase6_scenario scenario;
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

struct phase6_context {
	pjsip_endpoint *endpt;
	pj_caching_pool *caching_pool;
	pjsip_transport *server_udp;
	pjsip_transport *client_udp;
	pj_thread_t *event_thread;
	struct phase6_call *call;
	pjsip_regc *registration;
	pjsip_tp_state_callback previous_state_cb;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t timer_fired;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;
	atomic_t transport_shutdowns;
	atomic_t transport_destroys;
	atomic_t malformed_drops;
	atomic_t malformed_requests;
	atomic_t malformed_responses;
	atomic_t drop_next_invite;
	atomic_t registration_requests;
	atomic_t registration_callbacks;
	atomic_t registration_code;
	atomic_t options_requests;
	atomic_t options_callbacks;
	atomic_t options_code;
	atomic_t route_checks;
	atomic_t peak_transactions;
	atomic_t peak_timers;
	atomic_t allocated_pool_bytes;
	atomic_t peak_pool_bytes;
	atomic_t allocated_pool_blocks;
	atomic_t peak_pool_blocks;
	atomic_t peak_transports;
	atomic_t event_stack_status;
	atomic_t event_stack_unused;
	unsigned unused_port;
	pj_timer_entry response_timer;
	pjsip_inv_session *response_inv;
	int response_stage;
	int response_final_code;
};

static struct phase6_context *active_context;
static pjsip_module phase6_module;
static pjsip_module phase6_wire_module;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 6] FAIL %s:%d status=%d (%s)\n", test, line, status,
	       text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 6] FAIL %s:%d condition=%s\n", test, line, condition);
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
		    atomic_get(&active_context->callback_error) != 0) {
			if (active_context != NULL)
				printk("[Phase 6] wait abort callback=%d event=%d\n",
				       (int)atomic_get(&active_context->callback_error),
				       (int)atomic_get(&active_context->event_error));
			return -2;
		}
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
		    atomic_get(&active_context->callback_error) != 0) {
			if (active_context != NULL)
				printk("[Phase 6] wait abort callback=%d event=%d\n",
				       (int)atomic_get(&active_context->callback_error),
				       (int)atomic_get(&active_context->event_error));
			return -2;
		}
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_quiescence(struct phase6_context *context,
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

static const char *scenario_name(enum phase6_scenario scenario)
{
	static const char *const names[PHASE6_CALL_COUNT] = {
		"UAC BYE", "UAS BYE", "CANCEL/487", "486 rejection",
		"603 rejection", "dropped INVITE/retry", "unused-port timeout",
		"offerless INVITE", "re-INVITE/hold", "UAC close early",
		"UAS close confirmed",
	};

	return names[scenario];
}

static pj_status_t parse_sdp(pj_pool_t *pool, const char *direction,
			     pj_bool_t compatible, pj_bool_t uas,
			     pjmedia_sdp_session **session)
{
	static const char sendrecv[] =
		"v=0\r\n"
		"o=phase6 1 1 IN IP4 127.0.0.1\r\n"
		"s=phase6-loop-call\r\n"
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
		"o=phase6 2 2 IN IP4 127.0.0.1\r\n"
		"s=phase6-loop-call\r\n"
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
		"o=phase6 2 2 IN IP4 127.0.0.1\r\n"
		"s=phase6-loop-call\r\n"
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
		"o=phase6 3 3 IN IP4 127.0.0.1\r\n"
		"s=phase6-loop-call\r\n"
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
	{
		pj_status_t status = pjmedia_sdp_parse(pool, copy, length, session);

#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
		if (status == PJ_SUCCESS && compatible) {
			unsigned port = phase11_media_sdp_port(uas);

			if (port != 0)
				(*session)->media[0]->desc.port = port;
		}
#else
		PJ_UNUSED_ARG(uas);
#endif
		return status;
	}
}

static struct phase6_call *current_call(void)
{
	if (active_context == NULL || active_context->call == NULL) {
		record_callback_error(-100);
		return NULL;
	}
	return active_context->call;
}

static void on_state_changed(pjsip_inv_session *inv, pjsip_event *event)
{
	struct phase6_call *call = current_call();
	atomic_t *states;

	PJ_UNUSED_ARG(event);
	if (call == NULL)
		return;
	states = inv->role == PJSIP_ROLE_UAC ? &call->uac_states :
						  &call->uas_states;
	atomic_or(states, INV_STATE_BIT(inv->state));
	printk("[Phase 6] %s %s -> %s\n", scenario_name(call->scenario),
	       inv->role == PJSIP_ROLE_UAC ? "UAC" : "UAS",
	       pjsip_inv_state_name(inv->state));
}

static void on_media_update(pjsip_inv_session *inv, pj_status_t status)
{
	struct phase6_call *call = current_call();

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
	struct phase6_call *call = current_call();
	pjmedia_sdp_session *answer = NULL;
	const char *direction = "sendrecv";
	pj_status_t status;

	PJ_UNUSED_ARG(offer);
	if (call == NULL)
		return;
	if (call->scenario == PHASE6_REINVITE &&
	    atomic_get(&call->request_invite) >= 2)
		direction = "recvonly";
	status = parse_sdp(inv->pool_prov, direction, PJ_TRUE, PJ_TRUE, &answer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_set_sdp_answer(inv, answer);
	if (status != PJ_SUCCESS)
		record_callback_error(status);
}

static pj_status_t on_rx_reinvite(pjsip_inv_session *inv,
				  const pjmedia_sdp_session *offer,
				  pjsip_rx_data *rdata)
{
	struct phase6_call *call = current_call();

	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(offer);
	PJ_UNUSED_ARG(rdata);
	if (call == NULL)
		return PJ_EINVALIDOP;
	atomic_inc(&call->rx_reinvite);
	return PJ_EIGNORED;
}

static void count_response(struct phase6_call *call, int code,
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

static pj_bool_t phase6_on_rx_response(pjsip_rx_data *rdata)
{
	struct phase6_call *call;

	if (active_context == NULL)
		return PJ_FALSE;
	if (rdata->msg_info.cid != NULL &&
	    pj_strcmp2(&rdata->msg_info.cid->id,
		       "phase6-malformed-response") == 0) {
		pjsip_msg_body *body = rdata->msg_info.msg->body;
		pjmedia_sdp_session *sdp = NULL;
		pj_status_t status = PJ_EINVAL;

		if (body != NULL)
			status = pjmedia_sdp_parse(rdata->tp_info.pool, body->data,
						 body->len, &sdp);
		if (status == PJ_SUCCESS)
			record_callback_error(-210);
		else
			atomic_inc(&active_context->malformed_responses);
		return PJ_FALSE;
	}
	call = active_context->call;

	if (call != NULL &&
	    (rdata->tp_info.transport == active_context->client_udp ||
	     rdata->tp_info.transport == active_context->server_udp) &&
	    rdata->msg_info.cseq != NULL) {
		count_response(call, rdata->msg_info.msg->line.status.code,
			       rdata->msg_info.cseq->method.id);
		atomic_inc(&active_context->route_checks);
	}
	return PJ_FALSE;
}

static pj_status_t phase6_on_tx_response(pjsip_tx_data *tdata)
{
	struct phase6_call *call = active_context != NULL ?
				  active_context->call : NULL;
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
	printk("[Phase 6] wire TX response %d cseq-method=%d\n",
	       tdata->msg->line.status.code, cseq != NULL ? cseq->method.id : -1);
	return PJ_SUCCESS;
}

static pj_status_t phase6_on_tx_request(pjsip_tx_data *tdata)
{
	struct phase6_call *call = active_context != NULL ?
				  active_context->call : NULL;
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

static void phase6_response_timer(pj_timer_heap_t *timer_heap,
				  pj_timer_entry *entry)
{
	struct phase6_context *context = entry->user_data;
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

static pj_bool_t phase6_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase6_call *call = active_context != NULL ?
				  active_context->call : NULL;
	pjsip_dialog *dialog = NULL;
	pjsip_inv_session *invite = NULL;
	pjmedia_sdp_session *answer = NULL;
	pjsip_tpselector selector;
	char contact_text[96];
	pj_str_t contact;
	pjsip_method_e method;
	pj_status_t status;
	int final_code = 200;

	if (active_context == NULL || rdata->tp_info.transport !=
					 active_context->server_udp)
		return PJ_FALSE;
	method = rdata->msg_info.msg->line.req.method.id;
	if (method == PJSIP_REGISTER_METHOD || method == PJSIP_OPTIONS_METHOD) {
		pj_status_t reply_status;

		if (method == PJSIP_REGISTER_METHOD)
			atomic_inc(&active_context->registration_requests);
		else
			atomic_inc(&active_context->options_requests);
		reply_status = pjsip_endpt_respond_stateless(active_context->endpt,
							 rdata, 200, NULL, NULL,
							 NULL);
		if (reply_status != PJ_SUCCESS)
			record_callback_error(reply_status);
		return PJ_TRUE;
	}
	if (rdata->msg_info.cid != NULL &&
	    pj_strcmp2(&rdata->msg_info.cid->id,
		       "phase6-malformed-request") == 0) {
		pjsip_msg_body *body = rdata->msg_info.msg->body;
		pjmedia_sdp_session *sdp = NULL;
		pj_status_t parse_status = PJ_EINVAL;

		if (body != NULL)
			parse_status = pjmedia_sdp_parse(rdata->tp_info.pool,
							 body->data, body->len,
							 &sdp);
		if (parse_status == PJ_SUCCESS) {
			record_callback_error(-211);
		} else {
			atomic_inc(&active_context->malformed_requests);
			parse_status = pjsip_endpt_respond_stateless(
				active_context->endpt, rdata, 400, NULL, NULL, NULL);
			if (parse_status != PJ_SUCCESS)
				record_callback_error(parse_status);
		}
		return PJ_TRUE;
	}
	if (call == NULL)
		return PJ_FALSE;
	if (pj_ansi_snprintf(contact_text, sizeof(contact_text),
			     "sip:bob@127.0.0.1:%u;transport=udp",
			     active_context->server_udp->local_name.port) <= 0) {
		record_callback_error(-200);
		return PJ_TRUE;
	}
	contact = pj_str(contact_text);

	/* In-dialog requests are owned by the INVITE usage/dialog modules. */
	if (method != PJSIP_INVITE_METHOD || rdata->msg_info.to == NULL ||
	    rdata->msg_info.to->tag.slen != 0)
		return PJ_FALSE;
	if (atomic_cas(&active_context->drop_next_invite, 1, 0)) {
		printk("[Phase 6] deliberately dropped first INVITE datagram\n");
		return PJ_TRUE;
	}
	if (rdata->msg_info.via == NULL ||
	    rdata->msg_info.via->sent_by.port !=
			active_context->client_udp->local_name.port) {
		record_callback_error(-201);
		return PJ_TRUE;
	}
	{
		const pjsip_contact_hdr *contact =
			(const pjsip_contact_hdr *)pjsip_msg_find_hdr(
				rdata->msg_info.msg, PJSIP_H_CONTACT, NULL);
		const pjsip_sip_uri *contact_uri = (const pjsip_sip_uri *)
			(contact != NULL ? pjsip_uri_get_uri(contact->uri) : NULL);

		if (contact_uri == NULL || contact_uri->port !=
					active_context->client_udp->local_name.port) {
			record_callback_error(-202);
			return PJ_TRUE;
		}
	}
	atomic_inc(&active_context->route_checks);

	status = pjsip_dlg_create_uas_and_inc_lock(
		pjsip_ua_instance(), rdata, &contact, &dialog);
	if (status != PJ_SUCCESS) {
		record_callback_error(status);
		return PJ_TRUE;
	}
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = active_context->server_udp;
	status = pjsip_dlg_set_transport(dialog, &selector);
	if (status == PJ_SUCCESS)
		status = parse_sdp(dialog->pool, "sendrecv", PJ_TRUE, PJ_TRUE,
				   &answer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_create_uas(dialog, rdata, answer, 0, &invite);
	if (status == PJ_SUCCESS) {
		call->uas = invite;
		dialog->mod_data[phase6_module.id] = call;
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
	if (call->scenario == PHASE6_REJECT_4XX)
		final_code = PJSIP_SC_BUSY_HERE;
	else if (call->scenario == PHASE6_REJECT_6XX)
		final_code = PJSIP_SC_DECLINE;
	active_context->response_inv = invite;
	active_context->response_stage = 180;
	active_context->response_final_code =
		call->scenario == PHASE6_CANCEL ? 0 : final_code;
	pj_timer_entry_init(&active_context->response_timer, 180, active_context,
			    phase6_response_timer);
	{
		pj_time_val delay = {
			0, call->scenario == PHASE6_UAC_CLOSE_EARLY ? 250 : 20
		};

		status = pjsip_endpt_schedule_timer(active_context->endpt,
						    &active_context->response_timer,
						    &delay);
	}
	if (status != PJ_SUCCESS)
		record_callback_error(status);
	return PJ_TRUE;
}

static pjsip_module phase6_module = {
	.name = {"phase6-udp-call", 15},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_rx_request = phase6_on_rx_request,
};

static pjsip_module phase6_wire_module = {
	.name = {"phase6-wire-observer", 20},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_TRANSPORT_LAYER + 1,
	.on_rx_response = phase6_on_rx_response,
	.on_tx_request = phase6_on_tx_request,
	.on_tx_response = phase6_on_tx_response,
};

static void update_peak(atomic_t *peak, atomic_val_t sample)
{
	atomic_val_t previous = atomic_get(peak);

	while (sample > previous && !atomic_cas(peak, previous, sample))
		previous = atomic_get(peak);
}

static pj_bool_t phase6_on_block_alloc(pj_pool_factory *factory,
				       pj_size_t size)
{
	atomic_val_t bytes;
	atomic_val_t blocks;

	if (active_context == NULL || active_context->caching_pool == NULL ||
	    &active_context->caching_pool->factory != factory)
		return PJ_TRUE;
	bytes = atomic_add(&active_context->allocated_pool_bytes,
			   (atomic_val_t)size) + (atomic_val_t)size;
	blocks = atomic_inc(&active_context->allocated_pool_blocks) + 1;
	update_peak(&active_context->peak_pool_bytes, bytes);
	update_peak(&active_context->peak_pool_blocks, blocks);
	return PJ_TRUE;
}

static void phase6_on_block_free(pj_pool_factory *factory, pj_size_t size)
{
	if (active_context == NULL || active_context->caching_pool == NULL ||
	    &active_context->caching_pool->factory != factory)
		return;
	atomic_sub(&active_context->allocated_pool_bytes, (atomic_val_t)size);
	atomic_dec(&active_context->allocated_pool_blocks);
}

static void phase6_on_drop_data(pjsip_tp_dropped_data *data)
{
	if (active_context == NULL || data == NULL)
		return;
	if ((data->tp == active_context->server_udp ||
	     data->tp == active_context->client_udp) &&
	    data->status != PJ_SUCCESS)
		atomic_inc(&active_context->malformed_drops);
}

static void phase6_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	pjsip_tp_state_callback previous = NULL;

	if (active_context != NULL) {
		previous = active_context->previous_state_cb;
		if (transport->key.type == PJSIP_TRANSPORT_UDP) {
			if (state == PJSIP_TP_STATE_SHUTDOWN)
				atomic_inc(&active_context->transport_shutdowns);
			else if (state == PJSIP_TP_STATE_DESTROY)
				atomic_inc(&active_context->transport_destroys);
		}
	}
	if (previous != NULL && previous != phase6_on_transport_state)
		previous(transport, state, info);
}

static int phase6_event_thread(void *arg)
{
	struct phase6_context *context = arg;
	pj_time_val timeout = {0, 10};
	size_t unused = 0;
	int stack_status;

	atomic_set(&context->event_started, 1);
	while (!atomic_get(&context->event_stop)) {
		pj_status_t status = pjsip_endpt_handle_events(context->endpt,
							    &timeout);
		unsigned timers = pj_timer_heap_count(
			pjsip_endpt_get_timer_heap(context->endpt));

		atomic_inc(&context->event_polls);
		update_peak(&context->peak_transactions,
			    pjsip_tsx_layer_get_tsx_count());
		update_peak(&context->peak_timers, timers);
		update_peak(&context->peak_transports,
			    (context->server_udp != NULL ? 1 : 0) +
				    (context->client_udp != NULL ? 1 : 0));
		if (context->caching_pool != NULL)
		if (status != PJ_SUCCESS) {
			atomic_set(&context->event_error, status);
			break;
		}
	}
	stack_status = k_thread_stack_space_get(k_current_get(), &unused);
	atomic_set(&context->event_stack_status, stack_status);
	atomic_set(&context->event_stack_unused, (atomic_val_t)unused);
	return 0;
}

static void phase6_timer_callback(pj_timer_heap_t *timer_heap,
				  pj_timer_entry *entry)
{
	struct phase6_context *context = entry->user_data;

	PJ_UNUSED_ARG(timer_heap);
	atomic_set(&context->timer_fired, 1);
}

static void phase6_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static int test_idle_pump(struct phase6_context *context)
{
	const char *test = "loop transport idle event pump";
	pj_timer_entry timer;
	pj_time_val delay = {0, 25};

	pj_timer_entry_init(&timer, 1, context, phase6_timer_callback);
	CHECK_STATUS(test, pjsip_endpt_schedule_timer(context->endpt, &timer,
						     &delay));
	CHECK_TRUE(test, wait_for_value(&context->timer_fired, 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->event_polls) > 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	printk("[Phase 6] UDP event pump and endpoint timer: PASSED\n");
	return 0;
}

static int start_call(struct phase6_context *context, struct phase6_call *call,
		      pj_bool_t with_offer, pj_bool_t tolerate_send_error)
{
	const char *test = scenario_name(call->scenario);
	pjsip_dialog *dialog = NULL;
	pjsip_tpselector selector;
	pjmedia_sdp_session *offer = NULL;
	pjsip_tx_data *tdata = NULL;
	char local_text[96];
	char remote_text[96];
	pj_str_t local;
	pj_str_t remote;
	pj_status_t status;
	unsigned target_port = call->scenario == PHASE6_UDP_TIMEOUT ?
				context->unused_port :
				(unsigned)context->server_udp->local_name.port;

	context->call = call;
	if (pj_ansi_snprintf(local_text, sizeof(local_text),
			     "sip:alice@127.0.0.1:%u;transport=udp",
			     context->client_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(remote_text, sizeof(remote_text),
			     "sip:bob@127.0.0.1:%u;transport=udp",
			     target_port) <= 0)
		return fail_value(test, __LINE__, "UDP SIP URIs fit");
	local = pj_str(local_text);
	remote = pj_str(remote_text);
	status = pjsip_dlg_create_uac(pjsip_ua_instance(), &local, &local,
				      &remote, &remote, &dialog);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	/* Keep the zero-usage dialog alive across set_transport(). */
	pjsip_dlg_inc_lock(dialog);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	status = pjsip_dlg_set_transport(dialog, &selector);
	if (status == PJ_SUCCESS && with_offer)
		status = parse_sdp(dialog->pool, "sendrecv", PJ_TRUE, PJ_FALSE,
				   &offer);
	if (status == PJ_SUCCESS && offer != NULL)
		status = pjmedia_sdp_validate(offer);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_create_uac(dialog, offer, 0, &call->uac);
	if (status != PJ_SUCCESS) {
		pjsip_dlg_dec_lock(dialog);
		return fail_status(test, __LINE__, status);
	}
	dialog->mod_data[phase6_module.id] = call;
	pjsip_dlg_dec_lock(dialog);
	status = pjsip_inv_invite(call->uac, &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_inv_send_msg(call->uac, tdata);
	if (status != PJ_SUCCESS && !tolerate_send_error)
		return fail_status(test, __LINE__, status);
	return 0;
}

static int finish_call(struct phase6_context *context,
		       struct phase6_call *call, pjsip_inv_session *initiator)
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
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call->uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call->request_bye) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_200_bye) == 1);
	context->call = NULL;
	return 0;
}

static int wait_confirmed(struct phase6_call *call)
{
	const char *test = scenario_name(call->scenario);
	atomic_val_t confirmed = INV_STATE_BIT(PJSIP_INV_STATE_EARLY) |
				 INV_STATE_BIT(PJSIP_INV_STATE_CONFIRMED);

	CHECK_TRUE(test, wait_for_bits(&call->uac_states, confirmed,
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call->uas_states, confirmed,
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, call->scenario == PHASE6_DROP_RETRY ?
			 atomic_get(&call->request_invite) >= 2 :
			 atomic_get(&call->request_invite) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_100) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_180) == 1);
	CHECK_TRUE(test, atomic_get(&call->response_200_invite) == 1);
	CHECK_TRUE(test, atomic_get(&call->request_ack) == 1);
	CHECK_TRUE(test, atomic_get(&call->media_updates_uac) >= 1);
	CHECK_TRUE(test, atomic_get(&call->media_updates_uas) >= 1);
	CHECK_TRUE(test, atomic_get(&call->media_error) == 0);
	return 0;
}

static int test_connected_call(struct phase6_context *context,
			       enum phase6_scenario scenario)
{
	struct phase6_call call;
	int result;

	pj_bzero(&call, sizeof(call));
	call.scenario = scenario;
	context->call = &call;
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	CHECK_STATUS(scenario_name(scenario), phase11_media_prepare_call());
#endif
	CHECK_TRUE(scenario_name(scenario),
		   start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(scenario_name(scenario), wait_confirmed(&call) == 0);
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	CHECK_STATUS(scenario_name(scenario),
		     phase11_media_start_call(call.uac, call.uas));
	CHECK_STATUS(scenario_name(scenario), phase11_media_exercise_call());
#endif
	result = finish_call(context, &call,
			     scenario == PHASE6_UAS_BYE ? call.uas : call.uac);
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	if (phase11_media_stop_call() != PJ_SUCCESS)
		return fail_value(scenario_name(scenario), __LINE__,
				  "media stop after active BYE");
#endif
	CHECK_TRUE(scenario_name(scenario), result == 0);
	printk("[Phase 6] %s call flow: PASSED\n", scenario_name(scenario));
	return 0;
}

static int test_cancel(struct phase6_context *context)
{
	const char *test = "CANCEL/487";
	struct phase6_call call;
	pjsip_tx_data *tdata = NULL;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_CANCEL;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_EARLY),
				      PHASE6_WAIT_MS) == 0);
	CHECK_STATUS(test, pjsip_inv_end_session(call.uac,
						PJSIP_SC_REQUEST_TERMINATED,
						NULL, &tdata));
	CHECK_TRUE(test, tdata != NULL);
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_cancel) == 1);
	CHECK_TRUE(test, atomic_get(&call.response_200_cancel) == 1);
	CHECK_TRUE(test, atomic_get(&call.response_487) == 1);
	context->call = NULL;
	printk("[Phase 6] CANCEL -> 200 and INVITE -> 487: PASSED\n");
	return 0;
}

static int test_rejection(struct phase6_context *context,
			  enum phase6_scenario scenario)
{
	const char *test = scenario_name(scenario);
	struct phase6_call call;
	atomic_t *response;

	pj_bzero(&call, sizeof(call));
	call.scenario = scenario;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	response = scenario == PHASE6_REJECT_4XX ? &call.response_486 :
						      &call.response_603;
	CHECK_TRUE(test, atomic_get(response) == 1);
	context->call = NULL;
	printk("[Phase 6] %s: PASSED\n", test);
	return 0;
}

static int test_drop_retry(struct phase6_context *context)
{
	const char *test = "dropped INVITE/retry";
	struct phase6_call call;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_DROP_RETRY;
	context->call = &call;
	atomic_set(&context->drop_next_invite, 1);
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_invite) >= 2);
	CHECK_TRUE(test, finish_call(context, &call, call.uac) == 0);
	printk("[Phase 6] dropped INVITE and UDP retransmission recovery: PASSED\n");
	return 0;
}

static int test_udp_timeout(struct phase6_context *context)
{
	const char *test = "unused-port timeout";
	struct phase6_call call;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_UDP_TIMEOUT;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_TRUE) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, call.uas == NULL);
	context->call = NULL;
	printk("[Phase 6] unused loopback UDP port retransmission/timeout: PASSED\n");
	return 0;
}

static int test_offerless(struct phase6_context *context)
{
	const char *test = "offerless INVITE";
	struct phase6_call call;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_OFFERLESS;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_FALSE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_TRUE(test, finish_call(context, &call, call.uac) == 0);
	printk("[Phase 6] offerless INVITE with answer in ACK: PASSED\n");
	return 0;
}

static int test_reinvite(struct phase6_context *context)
{
	const char *test = "re-INVITE/hold";
	struct phase6_call call;
	pjmedia_sdp_session *offer = NULL;
	const pjmedia_sdp_session *active = NULL;
	pjsip_tx_data *tdata = NULL;

	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_REINVITE;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_STATUS(test, parse_sdp(call.uac->pool_prov, "sendonly", PJ_TRUE,
					    PJ_FALSE,
					    &offer));
	CHECK_STATUS(test, pjsip_inv_reinvite(call.uac, NULL, offer, &tdata));
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_value(&call.rx_reinvite, 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.wire_200_invite_count, 2,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.media_updates_uac, 2,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_value(&call.media_updates_uas, 2,
				       PHASE6_WAIT_MS) == 0);
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
					    PJ_FALSE,
					    &offer));
	CHECK_STATUS(test, pjsip_inv_reinvite(call.uac, NULL, offer, &tdata));
	CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_value(&call.response_500, 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_invite) == 3);
	CHECK_TRUE(test, atomic_get(&call.uac_states) &
			 INV_STATE_BIT(PJSIP_INV_STATE_CONFIRMED));
	CHECK_TRUE(test, finish_call(context, &call, call.uac) == 0);
	printk("[Phase 6] re-INVITE hold and incompatible SDP rejection: PASSED\n");
	return 0;
}

static void phase6_registration_cb(struct pjsip_regc_cbparam *param)
{
	struct phase6_context *context = param != NULL ? param->token : NULL;

	if (context == NULL) {
		record_callback_error(-220);
		return;
	}
	atomic_set(&context->registration_code, param->code);
	atomic_inc(&context->registration_callbacks);
}

static void phase6_options_cb(void *token, pjsip_event *event)
{
	struct phase6_context *context = token;

	if (context == NULL || event == NULL ||
	    event->type != PJSIP_EVENT_TSX_STATE ||
	    event->body.tsx_state.tsx == NULL) {
		record_callback_error(-221);
		return;
	}
	if (event->body.tsx_state.tsx->state == PJSIP_TSX_STATE_COMPLETED ||
	    event->body.tsx_state.tsx->state == PJSIP_TSX_STATE_TERMINATED) {
		atomic_set(&context->options_code,
			   event->body.tsx_state.tsx->status_code);
		atomic_cas(&context->options_callbacks, 0, 1);
	}
}

static pj_status_t send_registration(struct phase6_context *context,
				     pj_bool_t unregister)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	if (unregister)
		status = pjsip_regc_unregister(context->registration, &tdata);
	else
		status = pjsip_regc_register(context->registration, PJ_TRUE,
					      &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_send(context->registration, tdata);
	return status;
}

static pj_status_t start_registration(struct phase6_context *context)
{
	char registrar_text[96];
	char identity_text[96];
	char contact_text[112];
	pj_str_t registrar;
	pj_str_t identity;
	pj_str_t contact;
	pjsip_tpselector selector;
	pj_status_t status;

	if (pj_ansi_snprintf(registrar_text, sizeof(registrar_text),
			     "sip:127.0.0.1:%u;transport=udp",
			     context->server_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(identity_text, sizeof(identity_text),
			     "<sip:phase6@127.0.0.1:%u>",
			     context->client_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(contact_text, sizeof(contact_text),
			     "<sip:phase6@127.0.0.1:%u;transport=udp>",
			     context->client_udp->local_name.port) <= 0)
		return PJ_ETOOSMALL;
	registrar = pj_str(registrar_text);
	identity = pj_str(identity_text);
	contact = pj_str(contact_text);
	status = pjsip_regc_create(context->endpt, context,
				   phase6_registration_cb,
				   &context->registration);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_init(context->registration, &registrar,
					  &identity, &identity, 1, &contact, 30);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	if (status == PJ_SUCCESS)
		status = pjsip_regc_set_transport(context->registration,
						 &selector);
	if (status == PJ_SUCCESS)
		status = send_registration(context, PJ_FALSE);
	return status;
}

static pj_status_t send_options(struct phase6_context *context)
{
	char target_text[96];
	char source_text[96];
	pj_str_t target;
	pj_str_t source;
	pj_str_t call_id = pj_str("phase6-options");
	pjsip_tx_data *tdata = NULL;
	pjsip_tpselector selector;
	pj_status_t status;

	if (pj_ansi_snprintf(target_text, sizeof(target_text),
			     "sip:service@127.0.0.1:%u;transport=udp",
			     context->server_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(source_text, sizeof(source_text),
			     "<sip:phase6@127.0.0.1:%u>",
			     context->client_udp->local_name.port) <= 0)
		return PJ_ETOOSMALL;
	target = pj_str(target_text);
	source = pj_str(source_text);
	status = pjsip_endpt_create_request(
		context->endpt, &pjsip_options_method, &target, &source, &target,
		NULL, &call_id, 600, NULL, &tdata);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	if (status == PJ_SUCCESS)
		status = pjsip_tx_data_set_transport(tdata, &selector);
	if (status == PJ_SUCCESS)
		status = pjsip_endpt_send_request(context->endpt, tdata, -1,
						  context, phase6_options_cb);
	if (status != PJ_SUCCESS && tdata != NULL)
		pjsip_tx_data_dec_ref(tdata);
	return status;
}

static int test_signaling_concurrency(struct phase6_context *context)
{
	const char *test = "registration/OPTIONS/call concurrency";
	struct phase6_call call;
	pjsip_tx_data *tdata = NULL;

	CHECK_STATUS(test, start_registration(context));
	CHECK_TRUE(test, wait_for_value(&context->registration_callbacks, 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->registration_code) == 200);
	pj_bzero(&call, sizeof(call));
	call.scenario = PHASE6_UAC_BYE;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_confirmed(&call) == 0);
	CHECK_STATUS(test, send_options(context));
	CHECK_TRUE(test, wait_for_value(&context->options_callbacks, 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->options_code) == 200);
	CHECK_TRUE(test, atomic_get(&context->registration_requests) == 1);
	CHECK_TRUE(test, atomic_get(&context->options_requests) == 1);
	CHECK_STATUS(test, pjsip_inv_end_session(call.uac, PJSIP_SC_OK, NULL,
					       &tdata));
	if (tdata != NULL)
		CHECK_STATUS(test, pjsip_inv_send_msg(call.uac, tdata));
	CHECK_TRUE(test, wait_for_bits(&call.uac_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(&call.uas_states,
				      INV_STATE_BIT(PJSIP_INV_STATE_DISCONNECTED),
				      PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&call.request_bye) == 1);
	CHECK_TRUE(test, atomic_get(&call.response_200_bye) == 1);
	context->call = NULL;
	CHECK_STATUS(test, send_registration(context, PJ_TRUE));
	CHECK_TRUE(test, wait_for_value(&context->registration_callbacks, 2,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->registration_code) == 200);
	CHECK_TRUE(test, atomic_get(&context->registration_requests) == 2);
	CHECK_STATUS(test, pjsip_regc_destroy2(context->registration, PJ_TRUE));
	context->registration = NULL;
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	printk("[Phase 6] active registration + OPTIONS + confirmed call: PASSED\n");
	return 0;
}

static pj_status_t restart_udp_transport(struct phase6_context *context,
					 pj_bool_t server)
{
	pj_sockaddr_in address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pjsip_transport *transport = NULL;
	pj_status_t status;

	status = pj_sockaddr_in_init(&address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pjsip_udp_transport_start(context->endpt, &address, NULL,
						   1, &transport);
	if (status == PJ_SUCCESS) {
		if (server)
			context->server_udp = transport;
		else
			context->client_udp = transport;
	}
	return status;
}

static int terminate_both_sides(struct phase6_context *context,
				struct phase6_call *call)
{
	const char *test = scenario_name(call->scenario);
	pj_status_t status;

	if (call->uac != NULL) {
		status = pjsip_inv_terminate(call->uac,
					     PJSIP_SC_SERVICE_UNAVAILABLE,
					     PJ_FALSE);
		if (status != PJ_SUCCESS)
			return fail_status(test, __LINE__, status);
		call->uac = NULL;
	}
	if (call->uas != NULL) {
		status = pjsip_inv_terminate(call->uas,
					     PJSIP_SC_SERVICE_UNAVAILABLE,
					     PJ_FALSE);
		if (status != PJ_SUCCESS)
			return fail_status(test, __LINE__, status);
		call->uas = NULL;
	}
	CHECK_TRUE(test, wait_for_quiescence(context, PHASE6_WAIT_MS) == 0);
	context->call = NULL;
	return 0;
}

static int test_transport_shutdown(struct phase6_context *context,
				   pj_bool_t close_uac_early)
{
	const char *test = close_uac_early ? "UAC close early" :
						 "UAS close confirmed";
	struct phase6_call call;
	atomic_val_t destroys_before = atomic_get(&context->transport_destroys);
	atomic_val_t shutdowns_before = atomic_get(&context->transport_shutdowns);
	pjsip_transport *transport;

	pj_bzero(&call, sizeof(call));
	call.scenario = close_uac_early ? PHASE6_UAC_CLOSE_EARLY :
						 PHASE6_UAS_CLOSE_CONFIRMED;
	context->call = &call;
	CHECK_TRUE(test, start_call(context, &call, PJ_TRUE, PJ_FALSE) == 0);
	if (close_uac_early) {
		CHECK_TRUE(test, wait_for_bits(&call.uac_states,
					      INV_STATE_BIT(PJSIP_INV_STATE_EARLY),
					      PHASE6_WAIT_MS) == 0);
		transport = context->client_udp;
	} else {
		CHECK_TRUE(test, wait_confirmed(&call) == 0);
		transport = context->server_udp;
	}
	CHECK_STATUS(test, pjsip_transport_shutdown(transport));
	CHECK_TRUE(test, wait_for_value(&context->transport_shutdowns,
				       shutdowns_before + 1, PHASE6_WAIT_MS) == 0);
	if (close_uac_early && context->response_stage != 0) {
		pjsip_endpt_cancel_timer(context->endpt,
					 &context->response_timer);
		context->response_stage = 0;
		context->response_inv = NULL;
	}
	CHECK_TRUE(test, terminate_both_sides(context, &call) == 0);
	CHECK_TRUE(test, wait_for_value(&context->transport_destroys,
				       destroys_before + 1, PHASE6_WAIT_MS) == 0);
	if (close_uac_early)
		context->client_udp = NULL;
	else
		context->server_udp = NULL;
	CHECK_STATUS(test, restart_udp_transport(context, !close_uac_early));
	CHECK_TRUE(test, context->client_udp != NULL &&
			 context->server_udp != NULL &&
			 context->client_udp->local_name.port !=
				 context->server_udp->local_name.port);
	printk("[Phase 6] %s and deterministic call cleanup/restart: PASSED\n",
	       test);
	return 0;
}

static pj_status_t send_raw_datagram(pjsip_transport *destination,
				     const void *payload, pj_ssize_t length)
{
	pj_sock_t socket = PJ_INVALID_SOCKET;
	pj_ssize_t sent = length;
	pj_status_t status;

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &socket);
	if (status == PJ_SUCCESS)
		status = pj_sock_sendto(socket, payload, &sent, 0,
					&destination->local_addr,
					destination->addr_len);
	if (socket != PJ_INVALID_SOCKET)
		pj_sock_close(socket);
	if (status != PJ_SUCCESS)
		return status;
	return sent == length ? PJ_SUCCESS : PJ_EUNKNOWN;
}

static int test_malformed_sdp(struct phase6_context *context)
{
	const char *test = "malformed SIP/SDP datagrams";
	static const char parser_error[] =
		"INVITE missing-required-sip-version\r\n\r\n";
	static const char bad_sdp[] =
		"v=0\r\n"
		"o=broken missing fields\r\n"
		"m=audio not-a-port RTP/AVP 0\r\n";
	char request[768];
	char response[768];
	int request_length;
	int response_length;

	CHECK_STATUS(test, send_raw_datagram(context->server_udp, parser_error,
						 sizeof(parser_error) - 1));
	CHECK_TRUE(test, wait_for_value(&context->malformed_drops, 1,
				       PHASE6_WAIT_MS) == 0);
	request_length = pj_ansi_snprintf(
		request, sizeof(request),
		"INVITE sip:bob@127.0.0.1:%u SIP/2.0\r\n"
		"Via: SIP/2.0/UDP 127.0.0.1:%u;branch=%s-p6-bad-req\r\n"
		"From: <sip:alice@127.0.0.1>;tag=badreq\r\n"
		"To: <sip:bob@127.0.0.1>\r\n"
		"Call-ID: phase6-malformed-request\r\n"
		"CSeq: 1 INVITE\r\n"
		"Contact: <sip:alice@127.0.0.1:%u;transport=udp>\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %u\r\n\r\n%s",
		context->server_udp->local_name.port,
		context->client_udp->local_name.port, PJSIP_RFC3261_BRANCH_ID,
		context->client_udp->local_name.port,
		(unsigned)pj_ansi_strlen(bad_sdp), bad_sdp);
	CHECK_TRUE(test, request_length > 0 &&
			 request_length < (int)sizeof(request));
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, request,
						 request_length));
	CHECK_TRUE(test, wait_for_value(&context->malformed_requests, 1,
				       PHASE6_WAIT_MS) == 0);
	response_length = pj_ansi_snprintf(
		response, sizeof(response),
		"SIP/2.0 200 OK\r\n"
		"Via: SIP/2.0/UDP 127.0.0.1:%u;branch=%s-p6-bad-rsp\r\n"
		"From: <sip:alice@127.0.0.1>;tag=badresponse\r\n"
		"To: <sip:bob@127.0.0.1>;tag=peer\r\n"
		"Call-ID: phase6-malformed-response\r\n"
		"CSeq: 2 OPTIONS\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %u\r\n\r\n%s",
		context->client_udp->local_name.port, PJSIP_RFC3261_BRANCH_ID,
		(unsigned)pj_ansi_strlen(bad_sdp), bad_sdp);
	CHECK_TRUE(test, response_length > 0 &&
			 response_length < (int)sizeof(response));
	CHECK_STATUS(test, send_raw_datagram(context->client_udp, response,
						 response_length));
	CHECK_TRUE(test, wait_for_value(&context->malformed_responses, 1,
				       PHASE6_WAIT_MS) == 0);
	printk("[Phase 6] parser drop and malformed request/response SDP: PASSED\n");
	return 0;
}

static pj_status_t select_unused_loopback_port(unsigned *port)
{
	pj_sockaddr_in address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_sock_t socket = PJ_INVALID_SOCKET;
	int address_length = sizeof(address);
	pj_status_t status;

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &socket);
	if (status == PJ_SUCCESS)
		status = pj_sockaddr_in_init(&address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pj_sock_bind(socket, &address, sizeof(address));
	if (status == PJ_SUCCESS)
		status = pj_sock_getsockname(socket, &address, &address_length);
	if (status == PJ_SUCCESS)
		*port = pj_sockaddr_get_port(&address);
	if (socket != PJ_INVALID_SOCKET)
		pj_sock_close(socket);
	return status;
}

static int run_lifecycle(int iteration)
{
	struct phase6_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pjsip_transport *server_udp = NULL;
	pjsip_transport *client_udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pj_pool_t *thread_pool = NULL;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	char server_text[PJ_INET_ADDRSTRLEN];
	char client_text[PJ_INET_ADDRSTRLEN];
	pjsip_ua_init_param ua_param;
	pjsip_inv_callback inv_cb;
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t wire_module_registered = PJ_FALSE;
	pj_bool_t callbacks_installed = PJ_FALSE;
	pj_bool_t server_udp_started = PJ_FALSE;
	pj_bool_t client_udp_started = PJ_FALSE;
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	pj_bool_t phase11_media_initialized = PJ_FALSE;
#endif
	atomic_val_t destroy_target;
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
	active_context = &context;
	pj_caching_pool_init(&caching_pool, NULL, 0);
	caching_pool_initialized = PJ_TRUE;
	context.caching_pool = &caching_pool;
	caching_pool.factory.on_block_alloc = phase6_on_block_alloc;
	caching_pool.factory.on_block_free = phase6_on_block_free;
	status = pjsip_endpt_create(&caching_pool.factory, "phase6", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	status = pjsip_endpt_atexit(endpoint, phase6_endpoint_exit);
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
	status = pjsip_endpt_register_module(endpoint, &phase6_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;
	status = pjsip_endpt_register_module(endpoint, &phase6_wire_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module wire observer", __LINE__,
			    status);
		goto destroy_endpoint;
	}
	wire_module_registered = PJ_TRUE;
	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase6_on_transport_state);
	if (status != PJ_SUCCESS) {
		fail_status("transport state callback", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_tpmgr_set_drop_data_cb(transport_manager,
					     phase6_on_drop_data);
	if (status != PJ_SUCCESS) {
		fail_status("transport drop callback", __LINE__, status);
		goto destroy_endpoint;
	}
	callbacks_installed = PJ_TRUE;
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	status = phase11_media_lifecycle_init(&caching_pool.factory, endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("Phase 11 media lifecycle init", __LINE__, status);
		goto destroy_endpoint;
	}
	phase11_media_initialized = PJ_TRUE;
#endif
	status = select_unused_loopback_port(&context.unused_port);
	if (status == PJ_SUCCESS)
		status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
						   &server_udp);
	if (status != PJ_SUCCESS) {
		fail_status("server UDP transport", __LINE__, status);
		goto destroy_endpoint;
	}
	server_udp_started = PJ_TRUE;
	context.server_udp = server_udp;
	status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
					   &client_udp);
	if (status != PJ_SUCCESS) {
		fail_status("client UDP transport", __LINE__, status);
		goto destroy_endpoint;
	}
	client_udp_started = PJ_TRUE;
	context.client_udp = client_udp;
	pj_sockaddr_print(&server_udp->local_addr, server_text,
			  sizeof(server_text), 0);
	pj_sockaddr_print(&client_udp->local_addr, client_text,
			  sizeof(client_text), 0);
	if (pj_ansi_strcmp(server_text, "127.0.0.1") != 0 ||
	    pj_ansi_strcmp(client_text, "127.0.0.1") != 0 ||
	    server_udp->local_name.port == 0 || client_udp->local_name.port == 0 ||
	    server_udp->local_name.port == client_udp->local_name.port ||
	    server_udp->key.type != PJSIP_TRANSPORT_UDP ||
	    client_udp->key.type != PJSIP_TRANSPORT_UDP ||
	    !(server_udp->flag & PJSIP_TRANSPORT_DATAGRAM) ||
	    !(client_udp->flag & PJSIP_TRANSPORT_DATAGRAM) ||
	    pjsip_udp_transport_get_socket(server_udp) == PJ_INVALID_SOCKET ||
	    pjsip_udp_transport_get_socket(client_udp) == PJ_INVALID_SOCKET) {
		fail_value("UDP transports", __LINE__,
			   "two distinct IPv4 loopback UDP transports");
		goto destroy_endpoint;
	}
	printk("[Phase 6] UAC UDP %s:%u -> UAS UDP %s:%u: PASSED\n",
	       client_text, client_udp->local_name.port,
	       server_text, server_udp->local_name.port);
	thread_pool = pjsip_endpt_create_pool(endpoint, "phase6-thread", 4096,
					      4096);
	if (thread_pool == NULL) {
		fail_value("event thread pool", __LINE__, "thread_pool != NULL");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p6-event", phase6_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("pj_thread_create", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_value(&context.event_started, 1, PHASE6_WAIT_MS) != 0) {
		fail_value("event pump", __LINE__, "event thread started");
		goto destroy_endpoint;
	}

	if (test_idle_pump(&context) != 0 ||
	    test_malformed_sdp(&context) != 0 ||
	    test_connected_call(&context, PHASE6_UAC_BYE) != 0 ||
	    test_connected_call(&context, PHASE6_UAS_BYE) != 0 ||
	    test_cancel(&context) != 0 ||
	    test_rejection(&context, PHASE6_REJECT_4XX) != 0 ||
	    test_rejection(&context, PHASE6_REJECT_6XX) != 0 ||
	    test_drop_retry(&context) != 0 ||
	    test_udp_timeout(&context) != 0 ||
	    test_offerless(&context) != 0 || test_reinvite(&context) != 0 ||
	    test_signaling_concurrency(&context) != 0 ||
	    test_transport_shutdown(&context, PJ_TRUE) != 0 ||
	    test_transport_shutdown(&context, PJ_FALSE) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.callback_error) != 0 ||
	    atomic_get(&context.event_error) != 0 ||
	    atomic_get(&context.route_checks) == 0)
		goto destroy_endpoint;
	result = 0;

destroy_endpoint:
	client_udp = context.client_udp;
	server_udp = context.server_udp;
	context.call = NULL;
#if defined(CONFIG_PJMEDIA_PHASE11_CALL_TEST)
	if (phase11_media_initialized) {
		status = phase11_media_lifecycle_destroy();
		if (status != PJ_SUCCESS) {
			fail_status("Phase 11 media lifecycle destroy", __LINE__, status);
			result = -1;
		}
		phase11_media_initialized = PJ_FALSE;
	}
#endif
	if (context.registration != NULL) {
		status = pjsip_regc_destroy2(context.registration, PJ_TRUE);
		if (status != PJ_SUCCESS) {
			fail_status("registration cleanup", __LINE__, status);
			result = -1;
		}
		context.registration = NULL;
	}
	destroy_target = atomic_get(&context.transport_destroys) +
			 (client_udp_started && client_udp != NULL ? 1 : 0) +
			 (server_udp_started && server_udp != NULL ? 1 : 0);
	if (client_udp_started && client_udp != NULL) {
		status = pjsip_transport_shutdown(client_udp);
		if (status != PJ_SUCCESS) {
			fail_status("client UDP shutdown", __LINE__, status);
			result = -1;
		}
		client_udp_started = PJ_FALSE;
	}
	if (server_udp_started && server_udp != NULL) {
		status = pjsip_transport_shutdown(server_udp);
		if (status != PJ_SUCCESS) {
			fail_status("server UDP shutdown", __LINE__, status);
			result = -1;
		}
		server_udp_started = PJ_FALSE;
	}
	if (context.event_thread != NULL &&
	    wait_for_value(&context.transport_destroys, destroy_target,
			   PHASE6_WAIT_MS) != 0) {
		fail_value("UDP transport teardown", __LINE__,
			   "two UDP destroy callbacks");
		result = -1;
	}
	context.client_udp = NULL;
	context.server_udp = NULL;
	client_udp = NULL;
	server_udp = NULL;
	if (result == 0 &&
	    (atomic_get(&context.transport_shutdowns) != 4 ||
	     atomic_get(&context.transport_destroys) != 4)) {
		fail_value("UDP transport callbacks", __LINE__,
			   "four shutdown and four destroy callbacks");
		result = -1;
	}
	if (result == 0 && context.event_thread != NULL &&
	    wait_for_quiescence(&context, PHASE6_WAIT_MS) != 0) {
		fail_value("UDP shutdown drain", __LINE__,
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
	if (callbacks_installed) {
		pjsip_tpmgr_set_drop_data_cb(transport_manager, NULL);
		pjsip_tpmgr_set_state_cb(transport_manager,
					 context.previous_state_cb);
		callbacks_installed = PJ_FALSE;
	}
	if (module_registered) {
		status = pjsip_endpt_unregister_module(endpoint, &phase6_module);
		if (status != PJ_SUCCESS) {
			fail_status("module unregister", __LINE__, status);
			result = -1;
		}
	}
	if (wire_module_registered) {
		status = pjsip_endpt_unregister_module(endpoint,
							 &phase6_wire_module);
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
		printk("[Phase 6] teardown counts: tsx=%u dialogs=%u timers=%u\n",
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
	if (atomic_get(&context.allocated_pool_bytes) != 0 ||
	    atomic_get(&context.allocated_pool_blocks) != 0) {
		fail_value("PJ heap cleanup", __LINE__,
			   "allocated PJ pool blocks returned to zero");
		result = -1;
	}
	if (atomic_get(&context.event_stack_status) != 0) {
		fail_value("event stack watermark", __LINE__,
			   "k_thread_stack_space_get succeeded");
		result = -1;
	}
	printk("[Phase 6] resources: PJ heap peak=%u B/%d blocks; max transactions=%d, timers=%d, transports=%d, UDP sockets peak=3, PJSIP ioqueue handles=2\n",
	       (unsigned)atomic_get(&context.peak_pool_bytes),
	       (int)atomic_get(&context.peak_pool_blocks),
	       (int)atomic_get(&context.peak_transactions),
	       (int)atomic_get(&context.peak_timers),
	       (int)atomic_get(&context.peak_transports));
	printk("[Phase 6] event-thread stack: configured=%u B, used<=%u B, unused=%d B; network ceilings contexts=%u conns=%u open=%u poll=%u\n",
	       (unsigned)CONFIG_DYNAMIC_THREAD_STACK_SIZE,
	       (unsigned)(CONFIG_DYNAMIC_THREAD_STACK_SIZE -
			  atomic_get(&context.event_stack_unused)),
	       (int)atomic_get(&context.event_stack_unused),
	       (unsigned)CONFIG_NET_MAX_CONTEXTS, (unsigned)CONFIG_NET_MAX_CONN,
	       (unsigned)CONFIG_ZVFS_OPEN_MAX, (unsigned)CONFIG_ZVFS_POLL_MAX);
	if (result == 0)
		printk("[Phase 6] lifecycle %d complete teardown: PASSED\n",
		       iteration);
destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	return result;
}

int phase6_udp_call_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	size_t main_unused = 0;
	int stack_status;
	int iteration;

	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 320;
	printk("[Phase 6] IPv4 UDP INVITE call control (%d lifecycles)\n",
	       PHASE6_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE6_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 6 RESULT: FAILED at lifecycle %d\n", iteration);
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
	stack_status = k_thread_stack_space_get(k_current_get(), &main_unused);
	if (stack_status != 0) {
		printk("[Phase 6] main stack watermark failed: %d\n", stack_status);
		return 1;
	}
	printk("[Phase 6] main-thread stack: configured=%u B, used<=%u B, unused=%u B\n",
	       (unsigned)CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - main_unused),
	       (unsigned)main_unused);
	printk("PHASE 6 RESULT: PASSED (3 complete IPv4 UDP call lifecycles)\n");
	return 0;
}
