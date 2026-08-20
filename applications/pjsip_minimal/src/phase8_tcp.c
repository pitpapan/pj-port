#include <pjsip.h>
#include <pjsip/sip_transport_tcp.h>
#include <pjsip/sip_transport_udp.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE8_LIFECYCLES 2
#define PHASE8_WAIT_MS 2500
#define PHASE8_MAX_OUTGOING 4
#define PHASE8_MAX_INCOMING 8
#define PHASE8_RAW_MESSAGE_SIZE 768
#define PHASE8_RAW_RESPONSE_SIZE 8192
#define PHASE8_LARGE_BODY_SIZE 2048

#define STATE_BIT(state) ((atomic_val_t)1 << (state))

_Static_assert(PJ_HAS_TCP == 1, "Phase 8 requires PJLIB TCP support");
_Static_assert(PJ_HAS_IPV6 == 0, "Phase 8 requires IPv4-only PJLIB");
_Static_assert(PJSIP_MAX_TRANSPORTS == 16,
	       "Phase 8 expects the validated 16-transport limit");

enum phase8_scenario {
	PHASE8_SUCCESS,
	PHASE8_REUSE,
	PHASE8_RECONNECT,
	PHASE8_TIMEOUT,
	PHASE8_DISCONNECT,
	PHASE8_SCENARIO_COUNT,
};

struct phase8_context {
	pjsip_endpoint *endpt;
	pjsip_tpfactory *tcp_factory;
	pjsip_transport *udp;
	pj_thread_t *event_thread;
	pjsip_tp_state_callback previous_state_cb;

	pjsip_transport *outgoing[PHASE8_MAX_OUTGOING];
	pjsip_transport *incoming[PHASE8_MAX_INCOMING];
	pj_bool_t outgoing_held[PHASE8_MAX_OUTGOING];
	pj_bool_t incoming_held[PHASE8_MAX_INCOMING];
	pjsip_transport *uac_transport[PHASE8_SCENARIO_COUNT];
	pjsip_transport *request_transport[PHASE8_SCENARIO_COUNT];
	pjsip_transport *raw_stream_transport;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t teardown_started;
	atomic_t teardown_poll_retries;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;

	atomic_t tcp_connected;
	atomic_t tcp_outgoing_connected;
	atomic_t tcp_incoming_connected;
	atomic_t tcp_disconnected;
	atomic_t tcp_shutdowns;
	atomic_t tcp_destroys;
	atomic_t last_disconnect_status;
	atomic_t udp_shutdowns;
	atomic_t udp_destroys;

	atomic_t request_count[PHASE8_SCENARIO_COUNT];
	atomic_t uac_states[PHASE8_SCENARIO_COUNT];
	atomic_t uac_status[PHASE8_SCENARIO_COUNT];
	atomic_t uac_retransmits[PHASE8_SCENARIO_COUNT];
	atomic_t uas_states[PHASE8_SCENARIO_COUNT];
	atomic_t uas_status[PHASE8_SCENARIO_COUNT];

	atomic_t raw_fragment_requests;
	atomic_t raw_batch_requests;
	atomic_t raw_reset_requests;
	atomic_t raw_body_valid;
	atomic_t stream_rx_callbacks;
	atomic_t stream_rx_max_len;
};

static struct phase8_context *active_context;
static char large_response_body[PHASE8_LARGE_BODY_SIZE];

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 8] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 8] FAIL %s:%d condition=%s\n", test, line, condition);
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

static void record_callback_error(struct phase8_context *context, int error)
{
	atomic_cas(&context->callback_error, 0, error);
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

static int wait_for_count(struct phase8_context *context, atomic_t *value,
			  atomic_val_t minimum, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (atomic_get(value) < minimum) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_bits(struct phase8_context *context, atomic_t *value,
			 atomic_val_t expected, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while ((atomic_get(value) & expected) != expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_transactions(struct phase8_context *context,
				 unsigned expected, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (pjsip_tsx_layer_get_tsx_count() != expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_timers(struct phase8_context *context, unsigned expected,
			   unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (pj_timer_heap_count(pjsip_endpt_get_timer_heap(context->endpt)) !=
	       expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static enum phase8_scenario scenario_from_call_id(const pj_str_t *call_id)
{
	if (pj_strcmp2(call_id, "phase8-success") == 0)
		return PHASE8_SUCCESS;
	if (pj_strcmp2(call_id, "phase8-reuse") == 0)
		return PHASE8_REUSE;
	if (pj_strcmp2(call_id, "phase8-reconnect") == 0)
		return PHASE8_RECONNECT;
	if (pj_strcmp2(call_id, "phase8-timeout") == 0)
		return PHASE8_TIMEOUT;
	if (pj_strcmp2(call_id, "phase8-disconnect") == 0)
		return PHASE8_DISCONNECT;
	return PHASE8_SCENARIO_COUNT;
}

static pj_status_t send_transaction_response(struct phase8_context *context,
					     pjsip_transaction *uas,
					     pjsip_rx_data *rdata, int code)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_endpt_create_response(context->endpt, rdata, code, NULL,
					    &tdata);
	if (status != PJ_SUCCESS)
		return status;
	status = pjsip_tsx_send_msg(uas, tdata);
	if (status != PJ_SUCCESS)
		pjsip_tx_data_dec_ref(tdata);
	return status;
}

static pj_bool_t phase8_on_rx_request(pjsip_rx_data *rdata);
static void phase8_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event);

static pjsip_module phase8_module = {
	.name = {"phase8-validation", 17},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_UA_PROXY_LAYER - 1,
	.on_rx_request = phase8_on_rx_request,
	.on_tsx_state = phase8_on_tsx_state,
};

static pj_bool_t handle_raw_request(struct phase8_context *context,
				    pjsip_rx_data *rdata)
{
	const pj_str_t *call_id = &rdata->msg_info.cid->id;
	pjsip_msg_body *response_body = NULL;
	pj_status_t status;

	if (pj_strcmp2(call_id, "phase8-fragment-body") == 0) {
		static const char expected[] = "hello-stream";
		pjsip_msg_body *body = rdata->msg_info.msg->body;
		pj_str_t type = pj_str("text");
		pj_str_t subtype = pj_str("plain");
		pj_str_t text = {large_response_body, PHASE8_LARGE_BODY_SIZE};

		if (body == NULL || body->len != sizeof(expected) - 1 ||
		    pj_memcmp(body->data, expected, sizeof(expected) - 1) != 0) {
			record_callback_error(context, -201);
			return PJ_TRUE;
		}
		atomic_set(&context->raw_body_valid, 1);
		atomic_inc(&context->raw_fragment_requests);
		response_body = pjsip_msg_body_create(rdata->tp_info.pool, &type,
							  &subtype, &text);
		if (response_body == NULL) {
			record_callback_error(context, -202);
			return PJ_TRUE;
		}
	} else if (pj_strcmp2(call_id, "phase8-batch-one") == 0 ||
		   pj_strcmp2(call_id, "phase8-batch-two") == 0) {
		if (rdata->msg_info.msg->body != NULL) {
			record_callback_error(context, -203);
			return PJ_TRUE;
		}
		atomic_inc(&context->raw_batch_requests);
	} else if (pj_strcmp2(call_id, "phase8-reset") == 0) {
		atomic_inc(&context->raw_reset_requests);
	} else {
		return PJ_FALSE;
	}

	if (rdata->tp_info.transport->key.type != PJSIP_TRANSPORT_TCP) {
		record_callback_error(context, -204);
		return PJ_TRUE;
	}
	status = pjsip_endpt_respond_stateless(context->endpt, rdata, 200, NULL,
					      NULL, response_body);
	if (status != PJ_SUCCESS)
		record_callback_error(context, -205);
	return PJ_TRUE;
}

static pj_bool_t phase8_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase8_context *context = active_context;
	enum phase8_scenario scenario;
	pjsip_transaction *uas;
	pj_status_t status;

	if (context == NULL || rdata->msg_info.cid == NULL)
		return PJ_FALSE;
	if (handle_raw_request(context, rdata))
		return PJ_TRUE;

	scenario = scenario_from_call_id(&rdata->msg_info.cid->id);
	if (scenario == PHASE8_SCENARIO_COUNT)
		return PJ_FALSE;
	atomic_inc(&context->request_count[scenario]);
	context->request_transport[scenario] = rdata->tp_info.transport;
	if (rdata->tp_info.transport->key.type != PJSIP_TRANSPORT_TCP ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_OPTIONS_METHOD) {
		record_callback_error(context, -210);
		return PJ_TRUE;
	}
	if (scenario == PHASE8_TIMEOUT || scenario == PHASE8_DISCONNECT)
		return PJ_TRUE;

	status = pjsip_tsx_create_uas(&phase8_module, rdata, &uas);
	if (status != PJ_SUCCESS) {
		record_callback_error(context, -211);
		return PJ_TRUE;
	}
	uas->mod_data[phase8_module.id] = (void *)(uintptr_t)(scenario + 1);
	pjsip_tsx_recv_msg(uas, rdata);
	if (scenario == PHASE8_SUCCESS) {
		status = send_transaction_response(context, uas, rdata, 180);
		if (status == PJ_SUCCESS)
			status = send_transaction_response(context, uas, rdata, 202);
	} else {
		status = send_transaction_response(context, uas, rdata, 200);
	}
	if (status != PJ_SUCCESS)
		record_callback_error(context, -212);
	return PJ_TRUE;
}

static void phase8_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event)
{
	struct phase8_context *context = active_context;
	uintptr_t scenario_value;
	enum phase8_scenario scenario;

	if (context == NULL || phase8_module.id < 0)
		return;
	scenario_value = (uintptr_t)tsx->mod_data[phase8_module.id];
	if (scenario_value == 0 || scenario_value > PHASE8_SCENARIO_COUNT) {
		record_callback_error(context, -220);
		return;
	}
	scenario = (enum phase8_scenario)(scenario_value - 1);
	if (event->type == PJSIP_EVENT_TSX_STATE &&
	    event->body.tsx_state.type == PJSIP_EVENT_RX_MSG &&
	    event->body.tsx_state.src.rdata->tp_info.transport->key.type !=
		    PJSIP_TRANSPORT_TCP)
		record_callback_error(context, -221);

	if (tsx->role == PJSIP_ROLE_UAC) {
		atomic_or(&context->uac_states[scenario], STATE_BIT(tsx->state));
		if (tsx->transport != NULL &&
		    context->uac_transport[scenario] == NULL)
			context->uac_transport[scenario] = tsx->transport;
		if (tsx->state == PJSIP_TSX_STATE_PROCEEDING ||
		    tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uac_status[scenario], tsx->status_code);
		if (tsx->retransmit_count >
		    atomic_get(&context->uac_retransmits[scenario]))
			atomic_set(&context->uac_retransmits[scenario],
				   tsx->retransmit_count);
	} else {
		atomic_or(&context->uas_states[scenario], STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_PROCEEDING ||
		    tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uas_status[scenario], tsx->status_code);
	}
}

static void hold_connected_transport(struct phase8_context *context,
				     pjsip_transport *transport)
{
	pjsip_transport **array;
	pj_bool_t *held;
	atomic_t *count;
	unsigned maximum;
	unsigned index;

	if (transport->dir == PJSIP_TP_DIR_OUTGOING) {
		array = context->outgoing;
		held = context->outgoing_held;
		count = &context->tcp_outgoing_connected;
		maximum = PHASE8_MAX_OUTGOING;
	} else if (transport->dir == PJSIP_TP_DIR_INCOMING) {
		array = context->incoming;
		held = context->incoming_held;
		count = &context->tcp_incoming_connected;
		maximum = PHASE8_MAX_INCOMING;
	} else {
		record_callback_error(context, -230);
		return;
	}
	index = (unsigned)atomic_get(count);
	if (index >= maximum || pjsip_transport_add_ref(transport) != PJ_SUCCESS) {
		record_callback_error(context, -231);
		return;
	}
	array[index] = transport;
	held[index] = PJ_TRUE;
	atomic_inc(count);
	atomic_inc(&context->tcp_connected);
}

static void phase8_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	struct phase8_context *context = active_context;
	pjsip_tp_state_callback previous = NULL;

	if (context != NULL) {
		previous = context->previous_state_cb;
		if (transport->key.type == PJSIP_TRANSPORT_TCP) {
			if (state == PJSIP_TP_STATE_CONNECTED)
				hold_connected_transport(context, transport);
			else if (state == PJSIP_TP_STATE_DISCONNECTED) {
				atomic_inc(&context->tcp_disconnected);
				atomic_set(&context->last_disconnect_status,
					   info != NULL ? info->status : PJ_EUNKNOWN);
			} else if (state == PJSIP_TP_STATE_SHUTDOWN) {
				atomic_inc(&context->tcp_shutdowns);
			} else if (state == PJSIP_TP_STATE_DESTROY) {
				atomic_inc(&context->tcp_destroys);
			}
		} else if (transport == context->udp) {
			if (state == PJSIP_TP_STATE_SHUTDOWN)
				atomic_inc(&context->udp_shutdowns);
			else if (state == PJSIP_TP_STATE_DESTROY)
				atomic_inc(&context->udp_destroys);
		}
	}
	if (previous != NULL && previous != phase8_on_transport_state)
		previous(transport, state, info);
}

static pj_status_t phase8_on_stream_data(pjsip_tp_rx_data *data)
{
	struct phase8_context *context = active_context;
	atomic_val_t maximum;

	if (context == NULL || data->tp != context->raw_stream_transport)
		return PJ_SUCCESS;
	atomic_inc(&context->stream_rx_callbacks);
	maximum = atomic_get(&context->stream_rx_max_len);
	if ((atomic_val_t)data->len > maximum)
		atomic_set(&context->stream_rx_max_len, (atomic_val_t)data->len);
	return PJ_SUCCESS;
}

static int phase8_event_thread(void *arg)
{
	struct phase8_context *context = arg;

	atomic_set(&context->event_started, 1);
	while (!atomic_get(&context->event_stop)) {
		pj_time_val timeout = {0, 10};
		pj_status_t status = pjsip_endpt_handle_events(context->endpt,
							    &timeout);

		atomic_inc(&context->event_polls);
		if (status != PJ_SUCCESS) {
			if (atomic_get(&context->teardown_started) != 0 &&
			    status == PJ_STATUS_FROM_OS(EBADF)) {
				atomic_inc(&context->teardown_poll_retries);
				pj_thread_sleep(1);
				continue;
			}
			atomic_set(&context->event_error, status);
			break;
		}
	}
	return 0;
}

static void phase8_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static int start_options_transaction(struct phase8_context *context,
				     enum phase8_scenario scenario,
				     const char *call_id_text,
				     const char *branch_text)
{
	const char *test = "start TCP OPTIONS transaction";
	char target_text[96];
	char from_text[96];
	pj_str_t target;
	pj_str_t from;
	pj_str_t call_id = pj_str((char *)call_id_text);
	pjsip_tpselector selector;
	pjsip_tx_data *tdata = NULL;
	pjsip_via_hdr *via;
	pjsip_transaction *tsx = NULL;
	pj_status_t status;
	int length;

	length = pj_ansi_snprintf(target_text, sizeof(target_text),
				  "sip:service@127.0.0.1:%d;transport=tcp",
				  context->tcp_factory->addr_name.port);
	if (length <= 0 || length >= (int)sizeof(target_text))
		return fail_value(test, __LINE__, "target URI fits");
	length = pj_ansi_snprintf(from_text, sizeof(from_text),
				  "<sip:phase8@127.0.0.1:%d>",
				  context->tcp_factory->addr_name.port);
	if (length <= 0 || length >= (int)sizeof(from_text))
		return fail_value(test, __LINE__, "From URI fits");
	target = pj_str(target_text);
	from = pj_str(from_text);
	status = pjsip_endpt_create_request(context->endpt,
					    &pjsip_options_method, &target,
					    &from, &target, NULL, &call_id,
					    400 + scenario, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	via = (pjsip_via_hdr *)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA,
						  NULL);
	if (via == NULL) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_value(test, __LINE__, "Via header exists");
	}
	pj_strdup2(tdata->pool, &via->branch_param, branch_text);
	status = pjsip_tsx_create_uac(&phase8_module, tdata, &tsx);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	tsx->mod_data[phase8_module.id] = (void *)(uintptr_t)(scenario + 1);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_LISTENER;
	selector.u.listener = context->tcp_factory;
	status = pjsip_tsx_set_transport(tsx, &selector);
	if (status != PJ_SUCCESS) {
		pjsip_tsx_terminate(tsx, 500);
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	status = pjsip_tsx_send_msg(tsx, NULL);
	if (status != PJ_SUCCESS) {
		pjsip_tsx_terminate(tsx, 500);
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	return 0;
}

static int wait_success_transaction(struct phase8_context *context,
				    enum phase8_scenario scenario,
				    int expected_status,
				    pj_bool_t provisional)
{
	const char *test = "TCP OPTIONS transaction states";
	atomic_val_t uac_expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	atomic_val_t uas_expected = STATE_BIT(PJSIP_TSX_STATE_TRYING) |
		STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);

	if (provisional) {
		uac_expected |= STATE_BIT(PJSIP_TSX_STATE_PROCEEDING);
		uas_expected |= STATE_BIT(PJSIP_TSX_STATE_PROCEEDING);
	}
	CHECK_TRUE(test, wait_for_bits(context, &context->uac_states[scenario],
				       uac_expected, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(context, &context->uas_states[scenario],
				       uas_expected, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[scenario]) == 1);
	CHECK_TRUE(test, atomic_get(&context->uac_status[scenario]) ==
			 expected_status);
	CHECK_TRUE(test, atomic_get(&context->uas_status[scenario]) ==
			 expected_status);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits[scenario]) == 0);
	return 0;
}

static int test_connect_exchange_and_reuse(struct phase8_context *context)
{
	const char *test = "TCP async connect, accept, exchange, and reuse";
	pjsip_transport *client;
	pjsip_transport *server;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE8_SUCCESS, "phase8-success",
		PJSIP_RFC3261_BRANCH_ID "-phase8-success") == 0);
	CHECK_TRUE(test, wait_success_transaction(context, PHASE8_SUCCESS, 202,
						 PJ_TRUE) == 0);
	CHECK_TRUE(test, wait_for_count(context,
				       &context->tcp_outgoing_connected, 1,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_count(context,
				       &context->tcp_incoming_connected, 1,
				       PHASE8_WAIT_MS) == 0);
	client = context->outgoing[0];
	server = context->incoming[0];
	CHECK_TRUE(test, client != NULL && server != NULL && client != server);
	CHECK_TRUE(test, client->dir == PJSIP_TP_DIR_OUTGOING &&
			 server->dir == PJSIP_TP_DIR_INCOMING);
	CHECK_TRUE(test, pjsip_tcp_transport_get_socket(client) !=
			 PJ_INVALID_SOCKET);
	CHECK_TRUE(test, pjsip_tcp_transport_get_socket(server) !=
			 PJ_INVALID_SOCKET);
	CHECK_TRUE(test, context->uac_transport[PHASE8_SUCCESS] == client);
	CHECK_TRUE(test, context->request_transport[PHASE8_SUCCESS] == server);

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE8_REUSE, "phase8-reuse",
		PJSIP_RFC3261_BRANCH_ID "-phase8-reuse") == 0);
	CHECK_TRUE(test, wait_success_transaction(context, PHASE8_REUSE, 200,
						 PJ_FALSE) == 0);
	CHECK_TRUE(test, atomic_get(&context->tcp_connected) == 2);
	CHECK_TRUE(test, context->uac_transport[PHASE8_REUSE] == client);
	CHECK_TRUE(test, context->request_transport[PHASE8_REUSE] == server);
	printk("[Phase 8] async TCP connect/accept, OPTIONS 180 -> 202, and connection reuse: PASSED\n");
	return 0;
}

static void release_held_transport(struct phase8_context *context,
				   pjsip_transport *transport)
{
	unsigned i;

	for (i = 0; i < PHASE8_MAX_OUTGOING; ++i) {
		if (context->outgoing[i] == transport && context->outgoing_held[i]) {
			context->outgoing_held[i] = PJ_FALSE;
			pjsip_transport_dec_ref(transport);
			return;
		}
	}
	for (i = 0; i < PHASE8_MAX_INCOMING; ++i) {
		if (context->incoming[i] == transport && context->incoming_held[i]) {
			context->incoming_held[i] = PJ_FALSE;
			pjsip_transport_dec_ref(transport);
			return;
		}
	}
}

static int test_peer_close_and_reconnect(struct phase8_context *context)
{
	const char *test = "TCP peer close and reconnect";
	pjsip_transport *old_client = context->outgoing[0];
	pjsip_transport *old_server = context->incoming[0];
	atomic_val_t disconnected = atomic_get(&context->tcp_disconnected);
	atomic_val_t destroyed = atomic_get(&context->tcp_destroys);

	CHECK_STATUS(test, pjsip_transport_shutdown2(old_server, PJ_TRUE));
	release_held_transport(context, old_server);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 1, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_disconnected,
				       disconnected + 2, PHASE8_WAIT_MS) == 0);
	CHECK_STATUS(test, pjsip_transport_shutdown(old_client));
	release_held_transport(context, old_client);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 2, PHASE8_WAIT_MS) == 0);

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE8_RECONNECT, "phase8-reconnect",
		PJSIP_RFC3261_BRANCH_ID "-phase8-reconnect") == 0);
	CHECK_TRUE(test, wait_success_transaction(context, PHASE8_RECONNECT, 200,
						 PJ_FALSE) == 0);
	CHECK_TRUE(test, wait_for_count(context,
				       &context->tcp_outgoing_connected, 2,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_count(context,
				       &context->tcp_incoming_connected, 2,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, context->uac_transport[PHASE8_RECONNECT] ==
			 context->outgoing[1]);
	CHECK_TRUE(test, context->request_transport[PHASE8_RECONNECT] ==
			 context->incoming[1]);
	printk("[Phase 8] peer close, disconnect notification, and fresh reconnect: PASSED\n");
	return 0;
}

static pj_status_t connect_raw_peer(struct phase8_context *context,
				    pj_sock_t *socket, unsigned incoming_count,
				    pj_uint16_t *local_port)
{
	pj_sockaddr local;
	int length = sizeof(local);
	pj_status_t status;

	*socket = PJ_INVALID_SOCKET;
	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, socket);
	if (status != PJ_SUCCESS)
		return status;
	status = pj_sock_connect(*socket, &context->tcp_factory->local_addr,
				 pj_sockaddr_get_len(&context->tcp_factory->local_addr));
	if (status != PJ_SUCCESS)
		goto on_error;
	if (wait_for_count(context, &context->tcp_incoming_connected,
			   incoming_count, PHASE8_WAIT_MS) != 0) {
		status = PJ_ETIMEDOUT;
		goto on_error;
	}
	status = pj_sock_getsockname(*socket, &local, &length);
	if (status != PJ_SUCCESS)
		goto on_error;
	*local_port = (pj_uint16_t)pj_sockaddr_get_port(&local);
	return PJ_SUCCESS;

on_error:
	pj_sock_close(*socket);
	*socket = PJ_INVALID_SOCKET;
	return status;
}

static int build_raw_options(char *buffer, pj_size_t size, int target_port,
			     pj_uint16_t local_port, const char *call_id,
			     unsigned cseq, const char *body)
{
	pj_size_t body_len = body == NULL ? 0 : pj_ansi_strlen(body);
	int length = pj_ansi_snprintf(
		buffer, size,
		"OPTIONS sip:service@127.0.0.1:%d;transport=tcp SIP/2.0\r\n"
		"Via: SIP/2.0/TCP 127.0.0.1:%u;branch="
		PJSIP_RFC3261_BRANCH_ID "-%s\r\n"
		"From: <sip:raw@127.0.0.1>;tag=%u\r\n"
		"To: <sip:service@127.0.0.1>\r\n"
		"Call-ID: %s\r\n"
		"CSeq: %u OPTIONS\r\n"
		"Max-Forwards: 70\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: %u\r\n\r\n%s",
		target_port, (unsigned)local_port, call_id, cseq, call_id, cseq,
		(unsigned)body_len, body == NULL ? "" : body);

	return length > 0 && (pj_size_t)length < size ? length : -1;
}

static pj_status_t send_exact(pj_sock_t socket, const char *data,
			      pj_size_t length)
{
	pj_size_t sent_total = 0;

	while (sent_total < length) {
		pj_ssize_t sent = (pj_ssize_t)(length - sent_total);
		pj_status_t status = pj_sock_send(socket, data + sent_total, &sent, 0);

		if (status != PJ_SUCCESS)
			return status;
		if (sent <= 0)
			return PJ_EUNKNOWN;
		sent_total += (pj_size_t)sent;
	}
	return PJ_SUCCESS;
}

static unsigned count_pattern(const char *data, pj_size_t length,
			      const char *pattern)
{
	pj_size_t pattern_len = pj_ansi_strlen(pattern);
	pj_size_t i;
	unsigned count = 0;

	for (i = 0; i + pattern_len <= length; ++i) {
		if (pj_memcmp(data + i, pattern, pattern_len) == 0)
			++count;
	}
	return count;
}

static int receive_raw_responses(struct phase8_context *context,
				 pj_sock_t socket, unsigned expected,
				 pj_size_t *received, unsigned *chunks)
{
	char buffer[PHASE8_RAW_RESPONSE_SIZE];
	pj_time_val deadline;
	pj_size_t total = 0;
	unsigned read_count = 0;

	make_deadline(&deadline, PHASE8_WAIT_MS);
	while (count_pattern(buffer, total, "SIP/2.0 200") < expected) {
		pj_fd_set_t read_set;
		pj_time_val delay = {0, 50};
		pj_ssize_t length = 31;
		int ready;
		pj_status_t status;

		if (deadline_reached(&deadline) ||
		    atomic_get(&context->callback_error) != 0)
			return -1;
		PJ_FD_ZERO(&read_set);
		PJ_FD_SET(socket, &read_set);
		ready = pj_sock_select((int)socket + 1, &read_set, NULL, NULL,
				       &delay);
		if (ready < 0)
			return -1;
		if (ready == 0)
			continue;
		if (total + (pj_size_t)length > sizeof(buffer))
			return -1;
		status = pj_sock_recv(socket, buffer + total, &length, 0);
		if (status != PJ_SUCCESS || length <= 0)
			return -1;
		total += (pj_size_t)length;
		++read_count;
	}
	*received = total;
	*chunks = read_count;
	return 0;
}

static int test_stream_framing(struct phase8_context *context)
{
	const char *test = "TCP partial and coalesced stream framing";
	char fragment[PHASE8_RAW_MESSAGE_SIZE];
	char first[PHASE8_RAW_MESSAGE_SIZE];
	char second[PHASE8_RAW_MESSAGE_SIZE];
	char batch[PHASE8_RAW_MESSAGE_SIZE * 2];
	static const char body[] = "hello-stream";
	pj_sock_t peer = PJ_INVALID_SOCKET;
	pjsip_transport *server;
	pj_uint16_t local_port;
	pj_size_t header_length;
	pj_size_t response_bytes;
	unsigned response_chunks;
	atomic_val_t disconnected;
	atomic_val_t destroyed;
	int fragment_length;
	int first_length;
	int second_length;

	CHECK_STATUS(test, connect_raw_peer(context, &peer, 3, &local_port));
	server = context->incoming[2];
	CHECK_TRUE(test, server != NULL);
	context->raw_stream_transport = server;
	fragment_length = build_raw_options(
		fragment, sizeof(fragment), context->tcp_factory->addr_name.port,
		local_port, "phase8-fragment-body", 501, body);
	CHECK_TRUE(test, fragment_length > 0);
	header_length = (pj_size_t)fragment_length - (sizeof(body) - 1);
	CHECK_STATUS(test, send_exact(peer, fragment, 19));
	pj_thread_sleep(20);
	CHECK_TRUE(test, atomic_get(&context->raw_fragment_requests) == 0);
	CHECK_STATUS(test, send_exact(peer, fragment + 19,
				      header_length - 19 + 2));
	pj_thread_sleep(20);
	CHECK_TRUE(test, atomic_get(&context->raw_fragment_requests) == 0);
	CHECK_STATUS(test, send_exact(peer, fragment + header_length + 2,
				      sizeof(body) - 1 - 2));
	CHECK_TRUE(test, wait_for_count(context,
				       &context->raw_fragment_requests, 1,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->raw_body_valid) == 1);

	first_length = build_raw_options(
		first, sizeof(first), context->tcp_factory->addr_name.port,
		local_port, "phase8-batch-one", 502, NULL);
	second_length = build_raw_options(
		second, sizeof(second), context->tcp_factory->addr_name.port,
		local_port, "phase8-batch-two", 503, NULL);
	CHECK_TRUE(test, first_length > 0 && second_length > 0 &&
			 first_length + second_length < (int)sizeof(batch));
	pj_memcpy(batch, first, first_length);
	pj_memcpy(batch + first_length, second, second_length);
	CHECK_STATUS(test, send_exact(peer, batch, first_length + second_length));
	CHECK_TRUE(test, wait_for_count(context, &context->raw_batch_requests, 2,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->stream_rx_callbacks) >= 3);
	CHECK_TRUE(test, atomic_get(&context->stream_rx_max_len) <
			 fragment_length + first_length + second_length);
	CHECK_TRUE(test, receive_raw_responses(context, peer, 3, &response_bytes,
					      &response_chunks) == 0);
	CHECK_TRUE(test, response_bytes > PHASE8_LARGE_BODY_SIZE);
	CHECK_TRUE(test, response_chunks > 20);

	disconnected = atomic_get(&context->tcp_disconnected);
	destroyed = atomic_get(&context->tcp_destroys);
	CHECK_STATUS(test, pj_sock_close(peer));
	peer = PJ_INVALID_SOCKET;
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_disconnected,
				       disconnected + 1, PHASE8_WAIT_MS) == 0);
	context->raw_stream_transport = NULL;
	release_held_transport(context, server);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 1, PHASE8_WAIT_MS) == 0);
	printk("[Phase 8] chunked writes/partial reads, Content-Length body, three responses, and two coalesced messages: PASSED\n");
	return 0;
}

static int test_reset_path(struct phase8_context *context)
{
	const char *test = "TCP reset handling";
	char request[PHASE8_RAW_MESSAGE_SIZE];
	struct linger reset = {.l_onoff = 1, .l_linger = 0};
	pj_sock_t peer = PJ_INVALID_SOCKET;
	pjsip_transport *server;
	pj_uint16_t local_port;
	atomic_val_t disconnected;
	atomic_val_t destroyed;
	int length;
	int option_status;

	CHECK_STATUS(test, connect_raw_peer(context, &peer, 4, &local_port));
	server = context->incoming[3];
	CHECK_TRUE(test, server != NULL);
	length = build_raw_options(request, sizeof(request),
				   context->tcp_factory->addr_name.port, local_port,
				   "phase8-reset", 504, NULL);
	CHECK_TRUE(test, length > 0);
	CHECK_STATUS(test, send_exact(peer, request, length));
	CHECK_TRUE(test, wait_for_count(context, &context->raw_reset_requests, 1,
				       PHASE8_WAIT_MS) == 0);
	pj_thread_sleep(20);
	atomic_set(&context->last_disconnect_status, PJ_SUCCESS);
	disconnected = atomic_get(&context->tcp_disconnected);
	destroyed = atomic_get(&context->tcp_destroys);
	option_status = setsockopt((int)peer, SOL_SOCKET, SO_LINGER, &reset,
				   sizeof(reset));
	CHECK_TRUE(test, option_status == 0);
	CHECK_STATUS(test, pj_sock_close(peer));
	peer = PJ_INVALID_SOCKET;
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_disconnected,
				       disconnected + 1, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->last_disconnect_status) !=
			 PJ_SUCCESS);
	release_held_transport(context, server);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 1, PHASE8_WAIT_MS) == 0);
	printk("[Phase 8] TCP peer reset with unread response and safe transport destruction: PASSED\n");
	return 0;
}

static int test_timeout(struct phase8_context *context)
{
	const char *test = "TCP no-response transaction timeout";
	atomic_val_t expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE8_TIMEOUT, "phase8-timeout",
		PJSIP_RFC3261_BRANCH_ID "-phase8-timeout") == 0);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE8_TIMEOUT], expected,
				       PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE8_TIMEOUT]) == 1);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE8_TIMEOUT]) ==
			 PJSIP_SC_TSX_TIMEOUT);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits[PHASE8_TIMEOUT]) ==
			 0);
	CHECK_TRUE(test, context->uac_transport[PHASE8_TIMEOUT] ==
			 context->outgoing[1]);
	printk("[Phase 8] reliable-stream no-response transaction reached 408 timeout without retransmit: PASSED\n");
	return 0;
}

static int test_transaction_during_disconnect(struct phase8_context *context)
{
	const char *test = "active transaction during TCP disconnect";
	pjsip_transport *client = context->outgoing[1];
	pjsip_transport *server = context->incoming[1];
	atomic_val_t expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	atomic_val_t disconnected = atomic_get(&context->tcp_disconnected);
	atomic_val_t destroyed = atomic_get(&context->tcp_destroys);

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE8_DISCONNECT, "phase8-disconnect",
		PJSIP_RFC3261_BRANCH_ID "-phase8-disconnect") == 0);
	CHECK_TRUE(test, wait_for_count(
		context, &context->request_count[PHASE8_DISCONNECT], 1,
		PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, context->request_transport[PHASE8_DISCONNECT] == server);
	CHECK_STATUS(test, pjsip_transport_shutdown2(server, PJ_TRUE));
	release_held_transport(context, server);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_disconnected,
				       disconnected + 2, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(
		context, &context->uac_states[PHASE8_DISCONNECT], expected,
		PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE8_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE8_DISCONNECT]) ==
			 PJSIP_SC_TSX_TRANSPORT_ERROR);
	CHECK_TRUE(test,
		   atomic_get(&context->uac_retransmits[PHASE8_DISCONNECT]) == 0);
	CHECK_TRUE(test, context->uac_transport[PHASE8_DISCONNECT] == client);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 1, PHASE8_WAIT_MS) == 0);
	CHECK_STATUS(test, pjsip_transport_shutdown(client));
	release_held_transport(context, client);
	CHECK_TRUE(test, wait_for_count(context, &context->tcp_destroys,
				       destroyed + 2, PHASE8_WAIT_MS) == 0);
	printk("[Phase 8] active no-response transaction terminated with 503 transport error during disconnect: PASSED\n");
	return 0;
}

static void shutdown_held_tcp_transports(struct phase8_context *context)
{
	unsigned i;

	for (i = 0; i < PHASE8_MAX_OUTGOING; ++i) {
		if (context->outgoing_held[i])
			pjsip_transport_shutdown2(context->outgoing[i], PJ_TRUE);
	}
	for (i = 0; i < PHASE8_MAX_INCOMING; ++i) {
		if (context->incoming_held[i])
			pjsip_transport_shutdown2(context->incoming[i], PJ_TRUE);
	}
	for (i = 0; i < PHASE8_MAX_OUTGOING; ++i) {
		if (context->outgoing_held[i])
			release_held_transport(context, context->outgoing[i]);
	}
	for (i = 0; i < PHASE8_MAX_INCOMING; ++i) {
		if (context->incoming_held[i])
			release_held_transport(context, context->incoming[i]);
	}
}

static int run_lifecycle(int iteration)
{
	struct phase8_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pjsip_tpfactory *tcp_factory = NULL;
	pjsip_transport *udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pjsip_tcp_transport_cfg tcp_cfg;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t tsx_layer_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t callbacks_installed = PJ_FALSE;
	pj_bool_t tcp_factory_started = PJ_FALSE;
	pj_bool_t udp_started = PJ_FALSE;
	int tcp_send_buffer = 256;
	int wait_result;
	int result = -1;
	char tcp_text[PJ_INET_ADDRSTRLEN];
	char udp_text[PJ_INET_ADDRSTRLEN];

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
	status = pjsip_endpt_create(&caching_pool.factory, "phase8", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
	status = pjsip_endpt_atexit(endpoint, phase8_endpoint_exit);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_atexit", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_tsx_layer_init_module(endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tsx_layer_init_module", __LINE__, status);
		goto destroy_endpoint;
	}
	tsx_layer_initialized = PJ_TRUE;
	status = pjsip_endpt_register_module(endpoint, &phase8_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;

	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase8_on_transport_state);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_state_cb", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_tpmgr_set_recv_data_cb(transport_manager,
					     phase8_on_stream_data);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_recv_data_cb", __LINE__, status);
		goto destroy_endpoint;
	}
	callbacks_installed = PJ_TRUE;

	status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status != PJ_SUCCESS) {
		fail_status("pj_sockaddr_in_init loopback", __LINE__, status);
		goto destroy_endpoint;
	}
	pjsip_tcp_transport_cfg_default(&tcp_cfg, pj_AF_INET());
	pj_sockaddr_cp(&tcp_cfg.bind_addr, &bind_address);
	tcp_cfg.async_cnt = 1;
	tcp_cfg.initial_timeout = 2;
	tcp_cfg.sockopt_params.cnt = 1;
	tcp_cfg.sockopt_params.options[0].level = pj_SOL_SOCKET();
	tcp_cfg.sockopt_params.options[0].optname = pj_SO_SNDBUF();
	tcp_cfg.sockopt_params.options[0].optval = &tcp_send_buffer;
	tcp_cfg.sockopt_params.options[0].optlen = sizeof(tcp_send_buffer);
	status = pjsip_tcp_transport_start3(endpoint, &tcp_cfg, &tcp_factory);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tcp_transport_start3", __LINE__, status);
		goto destroy_endpoint;
	}
	tcp_factory_started = PJ_TRUE;
	context.tcp_factory = tcp_factory;
	status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
					   &udp);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_udp_transport_start", __LINE__, status);
		goto destroy_endpoint;
	}
	udp_started = PJ_TRUE;
	context.udp = udp;
	pj_sockaddr_print(&tcp_factory->local_addr, tcp_text, sizeof(tcp_text), 0);
	pj_sockaddr_print(&udp->local_addr, udp_text, sizeof(udp_text), 0);
	if (pj_ansi_strcmp(tcp_text, "127.0.0.1") != 0 ||
	    pj_ansi_strcmp(udp_text, "127.0.0.1") != 0 ||
	    tcp_factory->addr_name.port == 0 || udp->local_name.port == 0 ||
	    tcp_factory->type != PJSIP_TRANSPORT_TCP ||
	    !(tcp_factory->flag & PJSIP_TRANSPORT_RELIABLE) ||
	    udp->key.type != PJSIP_TRANSPORT_UDP ||
	    pjsip_udp_transport_get_socket(udp) == PJ_INVALID_SOCKET ||
	    pj_atomic_get(udp->ref_cnt) != 1) {
		fail_value("concurrent transports", __LINE__,
			   "IPv4 TCP listener and UDP transport are active");
		goto destroy_endpoint;
	}
	printk("[Phase 8] concurrent TCP listener %s:%d and UDP %s:%d within configured limits: PASSED\n",
	       tcp_text, tcp_factory->addr_name.port,
	       udp_text, udp->local_name.port);

	thread_pool = pjsip_endpt_create_pool(endpoint, "phase8-thread", 4096,
					      4096);
	if (thread_pool == NULL) {
		fail_value("event thread pool", __LINE__, "thread_pool != NULL");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p8-event", phase8_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("pj_thread_create event pump", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.event_started, 1,
			   PHASE8_WAIT_MS) != 0) {
		fail_value("event pump", __LINE__, "event thread started");
		goto destroy_endpoint;
	}

	if (test_connect_exchange_and_reuse(&context) != 0 ||
	    test_peer_close_and_reconnect(&context) != 0 ||
	    test_stream_framing(&context) != 0 ||
	    test_reset_path(&context) != 0 ||
	    test_timeout(&context) != 0 ||
	    test_transaction_during_disconnect(&context) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.callback_error) != 0 ||
	    atomic_get(&context.event_error) != 0) {
		printk("[Phase 8] callback/event error marker=%d status=%d\n",
		       (int)atomic_get(&context.callback_error),
		       (int)atomic_get(&context.event_error));
		goto destroy_endpoint;
	}
	result = 0;

destroy_endpoint:
	atomic_set(&context.teardown_started, 1);
	if (tcp_factory_started) {
		status = tcp_factory->destroy(tcp_factory);
		if (status != PJ_SUCCESS) {
			fail_status("TCP listener destroy", __LINE__, status);
			result = -1;
		}
		tcp_factory_started = PJ_FALSE;
		tcp_factory = NULL;
		context.tcp_factory = NULL;
	}
	shutdown_held_tcp_transports(&context);
	if (udp_started) {
		status = pjsip_transport_shutdown(udp);
		if (status != PJ_SUCCESS) {
			fail_status("UDP transport shutdown", __LINE__, status);
			result = -1;
		}
		udp_started = PJ_FALSE;
	}
	if (context.event_thread != NULL) {
		wait_result = wait_for_count(&context, &context.tcp_destroys,
					     atomic_get(&context.tcp_connected),
					     PHASE8_WAIT_MS);
		if (wait_result != 0) {
			printk("[Phase 8] TCP teardown diagnostics: wait=%d connected=%d destroyed=%d disconnected=%d shutdown=%d callback=%d event=%d\n",
			       wait_result, (int)atomic_get(&context.tcp_connected),
			       (int)atomic_get(&context.tcp_destroys),
			       (int)atomic_get(&context.tcp_disconnected),
			       (int)atomic_get(&context.tcp_shutdowns),
			       (int)atomic_get(&context.callback_error),
			       (int)atomic_get(&context.event_error));
			fail_value("TCP teardown", __LINE__,
				   "all connected transports destroyed while polling");
			result = -1;
		}
		if (udp != NULL &&
		    wait_for_count(&context, &context.udp_destroys, 1,
				   PHASE8_WAIT_MS) != 0) {
			fail_value("UDP teardown", __LINE__,
				   "UDP destroyed while polling");
			result = -1;
		}
		context.udp = NULL;
		udp = NULL;
		if (wait_for_transactions(&context, 0, PHASE8_WAIT_MS) != 0 ||
		    wait_for_timers(&context, 0, PHASE8_WAIT_MS) != 0) {
			fail_value("endpoint teardown", __LINE__,
				   "transactions and timers drained");
			result = -1;
		}
		pj_thread_sleep(PJ_IOQUEUE_KEY_FREE_DELAY + 50);
		atomic_set(&context.event_stop, 1);
		status = pj_thread_join(context.event_thread);
		if (status != PJ_SUCCESS) {
			fail_status("pj_thread_join event pump", __LINE__, status);
			result = -1;
		}
		status = pj_thread_destroy(context.event_thread);
		if (status != PJ_SUCCESS) {
			fail_status("pj_thread_destroy event pump", __LINE__, status);
			result = -1;
		}
		context.event_thread = NULL;
	}
	if (callbacks_installed) {
		pjsip_tpmgr_set_recv_data_cb(transport_manager, NULL);
		pjsip_tpmgr_set_state_cb(transport_manager,
					 context.previous_state_cb);
		callbacks_installed = PJ_FALSE;
	}
	if (module_registered &&
	    (!tsx_layer_initialized || pjsip_tsx_layer_get_tsx_count() == 0)) {
		status = pjsip_endpt_unregister_module(endpoint, &phase8_module);
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_endpt_unregister_module", __LINE__, status);
			result = -1;
		} else {
			module_registered = PJ_FALSE;
		}
	}
	if (tsx_layer_initialized && pjsip_tsx_layer_get_tsx_count() == 0) {
		status = pjsip_tsx_layer_destroy();
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_tsx_layer_destroy", __LINE__, status);
			result = -1;
		} else {
			tsx_layer_initialized = PJ_FALSE;
		}
	}
	if (thread_pool != NULL) {
		pj_pool_release(thread_pool);
		thread_pool = NULL;
	}
	pjsip_endpt_destroy(endpoint);
	endpoint = NULL;
	if (atomic_get(&context.endpoint_exit_count) != 1) {
		fail_value("endpoint exit callback", __LINE__,
			   "endpoint_exit_count == 1");
		result = -1;
	}
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("endpoint pool cleanup", __LINE__,
			   "used_count == 0 && capacity == 0");
		result = -1;
	}
	if (result == 0)
		printk("[Phase 8] lifecycle %d listener/transports/event/endpoint teardown: PASSED (select close-race retries=%d)\n",
		       iteration,
		       (int)atomic_get(&context.teardown_poll_retries));

destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 8] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase8_tcp_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	unsigned saved_keep_alive = pjsip_cfg()->tcp.keep_alive_interval;
	int iteration;
	int result = 0;

	pj_memset(large_response_body, 'R', sizeof(large_response_body));
	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 320;
	pjsip_cfg()->tcp.keep_alive_interval = 0;
	printk("[Phase 8] IPv4 TCP loopback validation (%d lifecycles)\n",
	       PHASE8_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE8_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 8 RESULT: FAILED at lifecycle %d\n", iteration);
			result = 1;
			break;
		}
	}
	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	pjsip_cfg()->tcp.keep_alive_interval = saved_keep_alive;
	if (result == 0)
		printk("PHASE 8 RESULT: PASSED (%d/%d lifecycles)\n",
		       PHASE8_LIFECYCLES, PHASE8_LIFECYCLES);
	return result;
}
