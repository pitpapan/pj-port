#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <stdint.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE7_LIFECYCLES 2
#define PHASE7_WAIT_MS 1800
#define PHASE7_OVERSIZED_LEN (PJSIP_MAX_PKT_LEN + 256)

#define STATE_BIT(state) ((atomic_val_t)1 << (state))

_Static_assert(PJ_HAS_IPV6 == 0, "Phase 7 requires IPv4-only PJLIB");
_Static_assert(PJSIP_MAX_TRANSPORTS == 16,
	       "Phase 7 capacity test expects 16 endpoint ioqueue handles");
_Static_assert(PJSIP_MAX_TRANSPORTS <= PJ_IOQUEUE_MAX_HANDLES,
	       "PJSIP transport limit exceeds the PJLIB ioqueue limit");

enum phase7_scenario {
	PHASE7_SUCCESS,
	PHASE7_RETRY,
	PHASE7_TIMEOUT,
	PHASE7_SCENARIO_COUNT,
};

struct phase7_context {
	pjsip_endpoint *endpt;
	pjsip_transport *client_udp;
	pjsip_transport *server_udp;
	pj_thread_t *event_thread;
	pjsip_tp_state_callback previous_state_cb;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;
	atomic_t malformed_drops;
	atomic_t oversized_drops;
	atomic_t last_drop_status;
	atomic_t transport_shutdowns;
	atomic_t transport_destroys;

	atomic_t request_count[PHASE7_SCENARIO_COUNT];
	atomic_t uac_states[PHASE7_SCENARIO_COUNT];
	atomic_t uac_status[PHASE7_SCENARIO_COUNT];
	atomic_t uac_retransmits[PHASE7_SCENARIO_COUNT];
	atomic_t uas_states[PHASE7_SCENARIO_COUNT];
	atomic_t uas_status[PHASE7_SCENARIO_COUNT];
};

static struct phase7_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 7] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 7] FAIL %s:%d condition=%s\n", test, line, condition);
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

static void record_callback_error(struct phase7_context *context, int error)
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

static int wait_for_count(struct phase7_context *context, atomic_t *value,
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

static int wait_for_bits(struct phase7_context *context, atomic_t *value,
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

static int wait_for_transactions(struct phase7_context *context,
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

static int wait_for_timers(struct phase7_context *context, unsigned expected,
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

static enum phase7_scenario scenario_from_call_id(const pj_str_t *call_id)
{
	if (pj_strcmp2(call_id, "phase7-success") == 0)
		return PHASE7_SUCCESS;
	if (pj_strcmp2(call_id, "phase7-retry") == 0)
		return PHASE7_RETRY;
	if (pj_strcmp2(call_id, "phase7-timeout") == 0)
		return PHASE7_TIMEOUT;
	return PHASE7_SCENARIO_COUNT;
}

static pj_status_t send_transaction_response(struct phase7_context *context,
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

static pj_bool_t phase7_on_rx_request(pjsip_rx_data *rdata);
static void phase7_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event);

static pjsip_module phase7_module = {
	.name = {"phase7-validation", 17},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_UA_PROXY_LAYER - 1,
	.on_rx_request = phase7_on_rx_request,
	.on_tsx_state = phase7_on_tsx_state,
};

static pj_bool_t phase7_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase7_context *context = active_context;
	enum phase7_scenario scenario;
	pjsip_transaction *uas;
	pj_status_t status;
	atomic_val_t request_number;

	if (context == NULL || rdata->msg_info.cid == NULL)
		return PJ_FALSE;

	scenario = scenario_from_call_id(&rdata->msg_info.cid->id);
	if (scenario == PHASE7_SCENARIO_COUNT)
		return PJ_FALSE;

	request_number = atomic_inc(&context->request_count[scenario]) + 1;
	if (rdata->tp_info.transport != context->server_udp ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_OPTIONS_METHOD) {
		record_callback_error(context, -101);
		return PJ_TRUE;
	}

	if (scenario == PHASE7_TIMEOUT ||
	    (scenario == PHASE7_RETRY && request_number == 1))
		return PJ_TRUE;

	if ((scenario == PHASE7_SUCCESS && request_number != 1) ||
	    (scenario == PHASE7_RETRY && request_number != 2)) {
		record_callback_error(context, -102);
		return PJ_TRUE;
	}

	status = pjsip_tsx_create_uas(&phase7_module, rdata, &uas);
	if (status != PJ_SUCCESS) {
		record_callback_error(context, -103);
		return PJ_TRUE;
	}
	uas->mod_data[phase7_module.id] = (void *)(uintptr_t)(scenario + 1);
	pjsip_tsx_recv_msg(uas, rdata);

	if (scenario == PHASE7_SUCCESS) {
		status = send_transaction_response(context, uas, rdata, 180);
		if (status != PJ_SUCCESS) {
			record_callback_error(context, -104);
			return PJ_TRUE;
		}
		status = send_transaction_response(context, uas, rdata, 202);
	} else {
		status = send_transaction_response(context, uas, rdata, 200);
	}
	if (status != PJ_SUCCESS)
		record_callback_error(context, -105);

	return PJ_TRUE;
}

static void phase7_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event)
{
	struct phase7_context *context = active_context;
	uintptr_t scenario_value;
	enum phase7_scenario scenario;

	if (context == NULL || phase7_module.id < 0)
		return;

	scenario_value = (uintptr_t)tsx->mod_data[phase7_module.id];
	if (scenario_value == 0 || scenario_value > PHASE7_SCENARIO_COUNT) {
		record_callback_error(context, -110);
		return;
	}
	scenario = (enum phase7_scenario)(scenario_value - 1);

	if (event->type == PJSIP_EVENT_TSX_STATE) {
		pjsip_transport *expected = tsx->role == PJSIP_ROLE_UAC ?
			context->client_udp : context->server_udp;

		if (event->body.tsx_state.type == PJSIP_EVENT_RX_MSG &&
		    event->body.tsx_state.src.rdata->tp_info.transport !=
			    expected)
			record_callback_error(context, -111);
		if (event->body.tsx_state.type == PJSIP_EVENT_TX_MSG &&
		    event->body.tsx_state.src.tdata->tp_info.transport != NULL &&
		    event->body.tsx_state.src.tdata->tp_info.transport !=
			    expected)
			record_callback_error(context, -112);
	}

	if (tsx->role == PJSIP_ROLE_UAC) {
		atomic_or(&context->uac_states[scenario], STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_PROCEEDING ||
		    tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uac_status[scenario], tsx->status_code);
		if (tsx->retransmit_count >
		    atomic_get(&context->uac_retransmits[scenario]))
			atomic_set(&context->uac_retransmits[scenario],
				   tsx->retransmit_count);
	} else {
		if (scenario == PHASE7_TIMEOUT)
			record_callback_error(context, -113);
		atomic_or(&context->uas_states[scenario], STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_PROCEEDING ||
		    tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uas_status[scenario], tsx->status_code);
	}
}

static void phase7_on_drop_data(pjsip_tp_dropped_data *data)
{
	struct phase7_context *context = active_context;

	if (context == NULL)
		return;
	if (data->tp != context->server_udp) {
		record_callback_error(context, -120);
		return;
	}
	if (data->status == PJ_SUCCESS) {
		record_callback_error(context, -121);
		return;
	}

	atomic_set(&context->last_drop_status, data->status);
	if (data->len >= PJSIP_MAX_PKT_LEN)
		atomic_inc(&context->oversized_drops);
	else
		atomic_inc(&context->malformed_drops);
}

static void phase7_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	struct phase7_context *context = active_context;
	pjsip_tp_state_callback previous = NULL;

	if (context != NULL) {
		previous = context->previous_state_cb;
		if (transport == context->client_udp ||
		    transport == context->server_udp) {
			if (state == PJSIP_TP_STATE_SHUTDOWN)
				atomic_inc(&context->transport_shutdowns);
			else if (state == PJSIP_TP_STATE_DESTROY)
				atomic_inc(&context->transport_destroys);
		}
	}
	if (previous != NULL && previous != phase7_on_transport_state)
		previous(transport, state, info);
}

static int phase7_event_thread(void *arg)
{
	struct phase7_context *context = arg;
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

static void phase7_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static int start_options_transaction(struct phase7_context *context,
				     enum phase7_scenario scenario,
				     const char *call_id_text,
				     const char *branch_text)
{
	const char *test = "start UDP OPTIONS transaction";
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
				  "sip:service@127.0.0.1:%d;transport=udp",
				  context->server_udp->local_name.port);
	if (length <= 0 || length >= (int)sizeof(target_text))
		return fail_value(test, __LINE__, "target URI fits");
	length = pj_ansi_snprintf(from_text, sizeof(from_text),
				  "<sip:phase7@127.0.0.1:%d>",
				  context->client_udp->local_name.port);
	if (length <= 0 || length >= (int)sizeof(from_text))
		return fail_value(test, __LINE__, "From URI fits");
	target = pj_str(target_text);
	from = pj_str(from_text);

	status = pjsip_endpt_create_request(context->endpt,
					    &pjsip_options_method, &target,
					    &from, &target, NULL, &call_id,
					    200 + scenario, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);

	via = (pjsip_via_hdr *)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA,
							  NULL);
	if (via == NULL) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_value(test, __LINE__, "Via header exists");
	}
	pj_strdup2(tdata->pool, &via->branch_param, branch_text);

	status = pjsip_tsx_create_uac(&phase7_module, tdata, &tsx);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	tsx->mod_data[phase7_module.id] = (void *)(uintptr_t)(scenario + 1);

	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
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

static int send_raw_datagram(pjsip_transport *udp, const void *payload,
			     pj_ssize_t payload_len)
{
	pj_sock_t sender = PJ_INVALID_SOCKET;
	pj_ssize_t sent = payload_len;
	pj_status_t status;

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &sender);
	if (status != PJ_SUCCESS)
		return status;
	status = pj_sock_sendto(sender, payload, &sent, 0, &udp->local_addr,
				udp->addr_len);
	pj_sock_close(sender);
	if (status != PJ_SUCCESS)
		return status;
	return sent == payload_len ? PJ_SUCCESS : PJ_EUNKNOWN;
}

static int test_malformed_datagrams(struct phase7_context *context)
{
	const char *test = "malformed and oversized UDP datagrams";
	static const char malformed[] =
		"OPTIONS sip:broken@127.0.0.1 SIP/2.0\r\n"
		"Broken Header Without Colon\r\n\r\n";
	char oversized[PHASE7_OVERSIZED_LEN];
	atomic_val_t polls_before = atomic_get(&context->event_polls);

	pj_memset(oversized, 'X', sizeof(oversized));
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, malformed,
					     sizeof(malformed) - 1));
	CHECK_TRUE(test, wait_for_count(context, &context->malformed_drops, 1,
					       PHASE7_WAIT_MS) == 0);
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, oversized,
					     sizeof(oversized)));
	CHECK_TRUE(test, wait_for_count(context, &context->oversized_drops, 1,
					       PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->last_drop_status) != PJ_SUCCESS);
	CHECK_TRUE(test, atomic_get(&context->event_polls) > polls_before);
	printk("[Phase 7] malformed and %u-byte oversized UDP datagrams: PASSED\n",
	       (unsigned int)sizeof(oversized));
	return 0;
}

static int test_successful_transaction(struct phase7_context *context)
{
	const char *test = "UDP OPTIONS provisional/final transaction";
	atomic_val_t uac_expected;
	atomic_val_t uas_expected;
	int wait_status;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE7_SUCCESS, "phase7-success",
		PJSIP_RFC3261_BRANCH_ID "-phase7-success") == 0);
	uac_expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		       STATE_BIT(PJSIP_TSX_STATE_PROCEEDING) |
		       STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		       STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		       STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	uas_expected = STATE_BIT(PJSIP_TSX_STATE_TRYING) |
		       STATE_BIT(PJSIP_TSX_STATE_PROCEEDING) |
		       STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		       STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		       STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	wait_status = wait_for_bits(context,
				    &context->uac_states[PHASE7_SUCCESS],
				    uac_expected, PHASE7_WAIT_MS);
	if (wait_status != 0) {
		printk("[Phase 7] success diagnostics wait=%d uac=0x%x uas=0x%x "
		       "requests=%d uac_status=%d uas_status=%d callback=%d event=%d "
		       "tsx=%u\n",
		       wait_status,
		       (unsigned int)atomic_get(&context->uac_states[PHASE7_SUCCESS]),
		       (unsigned int)atomic_get(&context->uas_states[PHASE7_SUCCESS]),
		       (int)atomic_get(&context->request_count[PHASE7_SUCCESS]),
		       (int)atomic_get(&context->uac_status[PHASE7_SUCCESS]),
		       (int)atomic_get(&context->uas_status[PHASE7_SUCCESS]),
		       (int)atomic_get(&context->callback_error),
		       (int)atomic_get(&context->event_error),
		       pjsip_tsx_layer_get_tsx_count());
		return fail_value(test, __LINE__, "expected UAC states");
	}
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uas_states[PHASE7_SUCCESS],
				       uas_expected, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE7_SUCCESS]) == 1);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE7_SUCCESS]) == 202);
	CHECK_TRUE(test, atomic_get(&context->uas_status[PHASE7_SUCCESS]) == 202);
	printk("[Phase 7] UDP OPTIONS UAC/UAS 180 -> 202: PASSED\n");
	return 0;
}

static int test_dropped_request(struct phase7_context *context)
{
	const char *test = "UDP dropped request retransmission";
	atomic_val_t uac_expected;
	atomic_val_t uas_expected;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE7_RETRY, "phase7-retry",
		PJSIP_RFC3261_BRANCH_ID "-phase7-retry") == 0);
	uac_expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		       STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		       STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		       STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	uas_expected = STATE_BIT(PJSIP_TSX_STATE_TRYING) |
		       STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		       STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		       STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE7_RETRY],
				       uac_expected, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uas_states[PHASE7_RETRY],
				       uas_expected, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE7_RETRY]) == 2);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits[PHASE7_RETRY]) >= 1);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE7_RETRY]) == 200);
	CHECK_TRUE(test, atomic_get(&context->uas_status[PHASE7_RETRY]) == 200);
	printk("[Phase 7] first-packet drop and UDP retransmission recovery: PASSED\n");
	return 0;
}

static int test_timeout(struct phase7_context *context)
{
	const char *test = "UDP no-response transaction timeout";
	atomic_val_t expected;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE7_TIMEOUT, "phase7-timeout",
		PJSIP_RFC3261_BRANCH_ID "-phase7-timeout") == 0);
	expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		   STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		   STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE7_TIMEOUT],
				       expected, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE7_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE7_TIMEOUT]) >= 3);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits[PHASE7_TIMEOUT]) >= 2);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE7_TIMEOUT]) ==
			 PJSIP_SC_TSX_TIMEOUT);
	printk("[Phase 7] UDP retransmissions and 408 no-response timeout: PASSED\n");
	return 0;
}

static const pj_ioqueue_callback empty_ioqueue_callbacks = {0};

static int test_ioqueue_capacity(struct phase7_context *context)
{
	const char *test = "endpoint ioqueue capacity";
	pj_ioqueue_key_t *keys[PJSIP_MAX_TRANSPORTS - 2];
	pj_sock_t sockets[PJSIP_MAX_TRANSPORTS - 2];
	pj_ioqueue_key_t *overflow_key = NULL;
	pj_sock_t overflow = PJ_INVALID_SOCKET;
	pj_pool_t *pool;
	pj_status_t status = PJ_SUCCESS;
	int registered = 0;
	int result = -1;
	int i;

	for (i = 0; i < PJSIP_MAX_TRANSPORTS - 2; ++i) {
		keys[i] = NULL;
		sockets[i] = PJ_INVALID_SOCKET;
	}
	pool = pjsip_endpt_create_pool(context->endpt, "phase7-limit", 8192,
					       4096);
	if (pool == NULL)
		return fail_value(test, __LINE__, "capacity pool exists");

	for (i = 0; i < PJSIP_MAX_TRANSPORTS - 2; ++i) {
		status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
					&sockets[i]);
		if (status != PJ_SUCCESS)
			goto cleanup;
		status = pj_ioqueue_register_sock(pool,
			pjsip_endpt_get_ioqueue(context->endpt), sockets[i], NULL,
			&empty_ioqueue_callbacks, &keys[i]);
		if (status != PJ_SUCCESS) {
			pj_sock_close(sockets[i]);
			sockets[i] = PJ_INVALID_SOCKET;
			goto cleanup;
		}
		registered++;
	}

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0, &overflow);
	if (status != PJ_SUCCESS)
		goto cleanup;
	status = pj_ioqueue_register_sock(pool,
		pjsip_endpt_get_ioqueue(context->endpt), overflow, NULL,
		&empty_ioqueue_callbacks, &overflow_key);
	if (status != PJ_ETOOMANY) {
		if (status == PJ_SUCCESS) {
			pj_ioqueue_unregister(overflow_key);
			overflow = PJ_INVALID_SOCKET;
		}
		fail_status(test, __LINE__, status);
		goto cleanup;
	}
	result = 0;

cleanup:
	if (overflow != PJ_INVALID_SOCKET)
		pj_sock_close(overflow);
	for (i = registered - 1; i >= 0; --i) {
		pj_status_t unregister_status = pj_ioqueue_unregister(keys[i]);

		if (unregister_status != PJ_SUCCESS) {
			fail_status(test, __LINE__, unregister_status);
			result = -1;
		}
	}
	pj_thread_sleep(PJ_IOQUEUE_KEY_FREE_DELAY + 50);
	pjsip_endpt_release_pool(context->endpt, pool);
	if (result != 0 && status != PJ_ETOOMANY)
		return fail_status(test, __LINE__, status);
	printk("[Phase 7] endpoint ioqueue limit=%u overflow status PJ_ETOOMANY: PASSED\n",
	       (unsigned int)PJSIP_MAX_TRANSPORTS);
	return result;
}

static int run_lifecycle(int iteration)
{
	struct phase7_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pjsip_transport *client_udp = NULL;
	pjsip_transport *server_udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t tsx_layer_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t callbacks_installed = PJ_FALSE;
	pj_bool_t client_udp_started = PJ_FALSE;
	pj_bool_t server_udp_started = PJ_FALSE;
	int result = -1;
	char client_text[PJ_INET_ADDRSTRLEN];
	char server_text[PJ_INET_ADDRSTRLEN];

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
	status = pjsip_endpt_create(&caching_pool.factory, "phase7", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
	status = pjsip_endpt_atexit(endpoint, phase7_endpoint_exit);
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
	status = pjsip_endpt_register_module(endpoint, &phase7_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;

	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase7_on_transport_state);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_state_cb", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_tpmgr_set_drop_data_cb(transport_manager,
					     phase7_on_drop_data);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_drop_data_cb", __LINE__, status);
		goto destroy_endpoint;
	}
	callbacks_installed = PJ_TRUE;

	status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status != PJ_SUCCESS) {
		fail_status("pj_sockaddr_in_init loopback", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
					   &server_udp);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_udp_transport_start server", __LINE__, status);
		goto destroy_endpoint;
	}
	server_udp_started = PJ_TRUE;
	context.server_udp = server_udp;
	status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
					   &client_udp);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_udp_transport_start client", __LINE__, status);
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
	    server_udp->local_name.port == 0 ||
	    client_udp->local_name.port == 0 ||
	    server_udp->local_name.port == client_udp->local_name.port ||
	    server_udp->key.type != PJSIP_TRANSPORT_UDP ||
	    client_udp->key.type != PJSIP_TRANSPORT_UDP ||
	    !(server_udp->flag & PJSIP_TRANSPORT_DATAGRAM) ||
	    !(client_udp->flag & PJSIP_TRANSPORT_DATAGRAM) ||
	    pjsip_udp_transport_get_socket(server_udp) == PJ_INVALID_SOCKET ||
	    pjsip_udp_transport_get_socket(client_udp) == PJ_INVALID_SOCKET ||
	    pj_atomic_get(server_udp->ref_cnt) != 1 ||
	    pj_atomic_get(client_udp->ref_cnt) != 1) {
		fail_value("UDP transports", __LINE__,
			   "distinct IPv4 loopback ephemeral ports, sockets, and refs");
		goto destroy_endpoint;
	}
	status = pjsip_transport_add_ref(server_udp);
	if (status != PJ_SUCCESS || pj_atomic_get(server_udp->ref_cnt) != 2) {
		fail_status("pjsip_transport_add_ref", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pjsip_transport_dec_ref(server_udp);
	if (status != PJ_SUCCESS || pj_atomic_get(server_udp->ref_cnt) != 1) {
		fail_status("pjsip_transport_dec_ref", __LINE__, status);
		goto destroy_endpoint;
	}
	printk("[Phase 7] UDP client %s:%d -> server %s:%d with add/ref release: PASSED\n",
	       client_text, client_udp->local_name.port,
	       server_text, server_udp->local_name.port);

	thread_pool = pjsip_endpt_create_pool(endpoint, "phase7-thread", 4096,
						      4096);
	if (thread_pool == NULL) {
		fail_value("event thread pool", __LINE__, "thread_pool != NULL");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p7-event", phase7_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("pj_thread_create event pump", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.event_started, 1,
			   PHASE7_WAIT_MS) != 0) {
		fail_value("event pump", __LINE__, "event thread started");
		goto destroy_endpoint;
	}

	if (test_malformed_datagrams(&context) != 0 ||
	    test_successful_transaction(&context) != 0 ||
	    test_dropped_request(&context) != 0 ||
	    test_timeout(&context) != 0 ||
	    test_ioqueue_capacity(&context) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.callback_error) != 0 ||
	    atomic_get(&context.event_error) != 0) {
		printk("[Phase 7] callback/event error marker=%d status=%d\n",
		       (int)atomic_get(&context.callback_error),
		       (int)atomic_get(&context.event_error));
		goto destroy_endpoint;
	}
	result = 0;

destroy_endpoint:
	if (result == 0 &&
	    ((client_udp_started && pj_atomic_get(client_udp->ref_cnt) != 1) ||
	     (server_udp_started && pj_atomic_get(server_udp->ref_cnt) != 1))) {
		fail_value("UDP transport teardown", __LINE__,
			   "permanent refs are the only remaining refs");
		result = -1;
	}
	if (client_udp_started) {
		status = pjsip_transport_shutdown(client_udp);
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_transport_shutdown client", __LINE__, status);
			result = -1;
		}
		client_udp_started = PJ_FALSE;
	}
	if (server_udp_started) {
		status = pjsip_transport_shutdown(server_udp);
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_transport_shutdown server", __LINE__, status);
			result = -1;
		}
		server_udp_started = PJ_FALSE;
	}
	if ((client_udp != NULL || server_udp != NULL) &&
	    context.event_thread != NULL &&
	    wait_for_count(&context, &context.transport_destroys,
			   (client_udp != NULL ? 1 : 0) +
			   (server_udp != NULL ? 1 : 0),
			   PHASE7_WAIT_MS) != 0) {
		fail_value("UDP transport teardown", __LINE__,
			   "destroy callbacks while event pump active");
		result = -1;
	}
	client_udp = NULL;
	server_udp = NULL;
	context.client_udp = NULL;
	context.server_udp = NULL;
	if (result == 0 &&
	    (atomic_get(&context.transport_shutdowns) != 2 ||
	     atomic_get(&context.transport_destroys) != 2)) {
		fail_value("UDP transport state", __LINE__,
			   "two shutdown and two destroy callbacks");
		result = -1;
	}
	if (result == 0 && context.event_thread != NULL &&
	    (wait_for_transactions(&context, 0, PHASE7_WAIT_MS) != 0 ||
	     wait_for_timers(&context, 0, PHASE7_WAIT_MS) != 0)) {
		fail_value("UDP transport teardown", __LINE__,
			   "transactions and timers drained");
		result = -1;
	}
	if (context.event_thread != NULL) {
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
		pjsip_tpmgr_set_drop_data_cb(transport_manager, NULL);
		pjsip_tpmgr_set_state_cb(transport_manager,
					 context.previous_state_cb);
		callbacks_installed = PJ_FALSE;
	}
	if (module_registered &&
	    (!tsx_layer_initialized || pjsip_tsx_layer_get_tsx_count() == 0)) {
		status = pjsip_endpt_unregister_module(endpoint, &phase7_module);
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
		printk("[Phase 7] lifecycle %d UDP/event/endpoint teardown: PASSED\n",
		       iteration);

destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 7] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase7_udp_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	int iteration;
	int result = 0;

	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 320;

	printk("[Phase 7] IPv4 UDP loopback validation (%d lifecycles)\n",
	       PHASE7_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE7_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 7 RESULT: FAILED at lifecycle %d\n", iteration);
			result = 1;
			break;
		}
	}

	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	if (result == 0)
		printk("PHASE 7 RESULT: PASSED (%d/%d lifecycles)\n",
		       PHASE7_LIFECYCLES, PHASE7_LIFECYCLES);
	return result;
}
