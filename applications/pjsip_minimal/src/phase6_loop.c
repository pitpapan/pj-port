#include <pjsip.h>
#include <pjsip/sip_transport_loop.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <stdint.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE6_LIFECYCLES 2
#define PHASE6_WAIT_MS 1600

#define STATE_BIT(state) ((atomic_val_t)1 << (state))

enum phase6_scenario {
	PHASE6_SUCCESS,
	PHASE6_TIMEOUT,
	PHASE6_CANCEL,
	PHASE6_SCENARIO_COUNT,
};

struct phase6_context {
	pjsip_endpoint *endpt;
	pjsip_transport *loop;
	pj_thread_t *event_thread;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t timer_fired;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;

	atomic_t request_count[PHASE6_SCENARIO_COUNT];
	atomic_t uac_states[PHASE6_SCENARIO_COUNT];
	atomic_t uac_status[PHASE6_SCENARIO_COUNT];
	atomic_t uac_retransmits[PHASE6_SCENARIO_COUNT];
	atomic_t uas_states;
	atomic_t uas_status;
};

static struct phase6_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 6] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
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

static void record_callback_error(struct phase6_context *context, int error)
{
	atomic_cas(&context->callback_error, 0, error);
}

static pj_bool_t deadline_reached(const pj_time_val *deadline)
{
	pj_time_val now;

	pj_gettimeofday(&now);
	return PJ_TIME_VAL_GTE(now, *deadline);
}

static void make_deadline(pj_time_val *deadline, unsigned timeout_ms)
{
	pj_gettimeofday(deadline);
	deadline->msec += timeout_ms;
	pj_time_val_normalize(deadline);
}

static int wait_for_bits(struct phase6_context *context, atomic_t *value,
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

static int wait_for_count(struct phase6_context *context, atomic_t *value,
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

static int wait_for_transactions(struct phase6_context *context,
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

static int wait_for_timers(struct phase6_context *context, unsigned expected,
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

static enum phase6_scenario scenario_from_call_id(const pj_str_t *call_id)
{
	if (pj_strcmp2(call_id, "phase6-success") == 0)
		return PHASE6_SUCCESS;
	if (pj_strcmp2(call_id, "phase6-timeout") == 0)
		return PHASE6_TIMEOUT;
	return PHASE6_CANCEL;
}

static pj_bool_t is_phase6_call_id(const pj_str_t *call_id)
{
	return pj_strcmp2(call_id, "phase6-success") == 0 ||
	       pj_strcmp2(call_id, "phase6-timeout") == 0 ||
	       pj_strcmp2(call_id, "phase6-cancel") == 0;
}

static pj_status_t send_transaction_response(struct phase6_context *context,
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

static pj_bool_t phase6_on_rx_request(pjsip_rx_data *rdata);
static void phase6_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event);

static pjsip_module phase6_module = {
	.name = {"phase6-validation", 17},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_UA_PROXY_LAYER - 1,
	.on_rx_request = phase6_on_rx_request,
	.on_tsx_state = phase6_on_tsx_state,
};

static pj_bool_t phase6_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase6_context *context = active_context;
	enum phase6_scenario scenario;
	pjsip_transaction *uas;
	pj_status_t status;

	if (context == NULL || rdata->msg_info.cid == NULL ||
	    !is_phase6_call_id(&rdata->msg_info.cid->id))
		return PJ_FALSE;

	scenario = scenario_from_call_id(&rdata->msg_info.cid->id);
	atomic_inc(&context->request_count[scenario]);

	if (rdata->tp_info.transport != context->loop ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_OPTIONS_METHOD) {
		record_callback_error(context, -101);
		return PJ_TRUE;
	}

	if (scenario != PHASE6_SUCCESS)
		return PJ_TRUE;

	if (atomic_get(&context->request_count[scenario]) != 1) {
		record_callback_error(context, -102);
		return PJ_TRUE;
	}

	status = pjsip_tsx_create_uas(&phase6_module, rdata, &uas);
	if (status != PJ_SUCCESS) {
		record_callback_error(context, -103);
		return PJ_TRUE;
	}
	uas->mod_data[phase6_module.id] =
		(void *)(uintptr_t)(PHASE6_SUCCESS + 1);
	pjsip_tsx_recv_msg(uas, rdata);

	status = send_transaction_response(context, uas, rdata, 180);
	if (status != PJ_SUCCESS) {
		record_callback_error(context, -104);
		return PJ_TRUE;
	}
	status = send_transaction_response(context, uas, rdata, 202);
	if (status != PJ_SUCCESS)
		record_callback_error(context, -105);

	return PJ_TRUE;
}

static void phase6_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event)
{
	struct phase6_context *context = active_context;
	uintptr_t scenario_value;
	enum phase6_scenario scenario;

	if (context == NULL || phase6_module.id < 0)
		return;

	scenario_value = (uintptr_t)tsx->mod_data[phase6_module.id];
	if (scenario_value == 0 || scenario_value > PHASE6_SCENARIO_COUNT) {
		record_callback_error(context, -110);
		return;
	}
	scenario = (enum phase6_scenario)(scenario_value - 1);

	if (event->type == PJSIP_EVENT_TSX_STATE) {
		if (event->body.tsx_state.type == PJSIP_EVENT_RX_MSG &&
		    event->body.tsx_state.src.rdata->tp_info.transport !=
			    context->loop)
			record_callback_error(context, -111);
		if (event->body.tsx_state.type == PJSIP_EVENT_TX_MSG &&
		    event->body.tsx_state.src.tdata->tp_info.transport != NULL &&
		    event->body.tsx_state.src.tdata->tp_info.transport !=
			    context->loop)
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
		if (scenario != PHASE6_SUCCESS)
			record_callback_error(context, -113);
		atomic_or(&context->uas_states, STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_PROCEEDING ||
		    tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uas_status, tsx->status_code);
	}
}

static int phase6_event_thread(void *arg)
{
	struct phase6_context *context = arg;
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

static int start_options_transaction(struct phase6_context *context,
				     enum phase6_scenario scenario,
				     const char *call_id_text,
				     const char *branch_text,
				     pjsip_transaction **transaction)
{
	const char *test = "start OPTIONS transaction";
	pj_str_t target = pj_str("sip:service@127.0.0.1;transport=loop-dgram");
	pj_str_t from = pj_str("<sip:phase6@127.0.0.1>");
	pj_str_t call_id = pj_str((char *)call_id_text);
	pjsip_tpselector selector;
	pjsip_tx_data *tdata = NULL;
	pjsip_via_hdr *via;
	pjsip_transaction *tsx = NULL;
	pj_status_t status;

	status = pjsip_endpt_create_request(context->endpt,
					    &pjsip_options_method, &target,
					    &from, &target, NULL, &call_id,
					    100 + scenario, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);

	via = (pjsip_via_hdr *)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA,
						  NULL);
	if (via == NULL) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_value(test, __LINE__, "Via header exists");
	}
	pj_strdup2(tdata->pool, &via->branch_param, branch_text);

	status = pjsip_tsx_create_uac(&phase6_module, tdata, &tsx);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	tsx->mod_data[phase6_module.id] = (void *)(uintptr_t)(scenario + 1);

	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->loop;
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

	*transaction = tsx;
	return 0;
}

static int test_event_timer(struct phase6_context *context)
{
	const char *test = "endpoint event timer";
	pj_timer_entry timer;
	pj_time_val delay = {0, 25};

	pj_timer_entry_init(&timer, 1, context, phase6_timer_callback);
	CHECK_STATUS(test, pjsip_endpt_schedule_timer(context->endpt, &timer,
						     &delay));
	CHECK_TRUE(test, wait_for_count(context, &context->timer_fired, 1,
					       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->event_polls) > 0);
	printk("[Phase 6] dedicated endpoint event pump and timer: PASSED\n");
	return 0;
}

static int test_successful_transaction(struct phase6_context *context)
{
	const char *test = "loop OPTIONS provisional/final transaction";
	pjsip_transaction *uac;
	atomic_val_t uac_expected;
	atomic_val_t uas_expected;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE6_SUCCESS, "phase6-success",
		PJSIP_RFC3261_BRANCH_ID "-phase6-success", &uac) == 0);
	PJ_UNUSED_ARG(uac);

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
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE6_SUCCESS],
				       uac_expected, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(context, &context->uas_states,
				       uas_expected, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE6_SUCCESS]) == 1);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE6_SUCCESS]) == 202);
	CHECK_TRUE(test, atomic_get(&context->uas_status) == 202);
	printk("[Phase 6] OPTIONS UAC/UAS 180 -> 202 state transitions: PASSED\n");
	return 0;
}

static int test_retransmit_timeout(struct phase6_context *context)
{
	const char *test = "transaction retransmission and timeout";
	pjsip_transaction *uac;
	atomic_val_t expected;

	CHECK_TRUE(test, start_options_transaction(
		context, PHASE6_TIMEOUT, "phase6-timeout",
		PJSIP_RFC3261_BRANCH_ID "-phase6-timeout", &uac) == 0);
	PJ_UNUSED_ARG(uac);

	expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		   STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		   STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE6_TIMEOUT],
				       expected, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->request_count[PHASE6_TIMEOUT]) >= 3);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits[PHASE6_TIMEOUT]) >= 2);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE6_TIMEOUT]) ==
			 PJSIP_SC_TSX_TIMEOUT);
	printk("[Phase 6] retransmission timers and 408 timeout: PASSED\n");
	return 0;
}

static int test_active_termination(struct phase6_context *context)
{
	const char *test = "active event-loop transaction termination";
	pjsip_transaction *uac;
	atomic_val_t expected;

	CHECK_STATUS(test, pjsip_loop_set_recv_delay(context->loop, 120, NULL));
	CHECK_TRUE(test, start_options_transaction(
		context, PHASE6_CANCEL, "phase6-cancel",
		PJSIP_RFC3261_BRANCH_ID "-phase6-cancel", &uac) == 0);
	CHECK_STATUS(test, pjsip_tsx_terminate(
		uac, PJSIP_SC_REQUEST_TERMINATED));

	expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		   STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		   STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	CHECK_TRUE(test, wait_for_bits(context,
				       &context->uac_states[PHASE6_CANCEL],
				       expected, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_count(context,
				       &context->request_count[PHASE6_CANCEL], 1,
				       PHASE6_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->uac_status[PHASE6_CANCEL]) ==
			 PJSIP_SC_REQUEST_TERMINATED);
	CHECK_STATUS(test, pjsip_loop_set_recv_delay(context->loop, 5, NULL));
	pj_thread_sleep(140);
	printk("[Phase 6] transaction termination during active polling: PASSED\n");
	return 0;
}

static int run_lifecycle(int iteration)
{
	struct phase6_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pjsip_transport *loop = NULL;
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t tsx_layer_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
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
	status = pjsip_endpt_create(&caching_pool.factory, "phase6", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
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
	tsx_layer_initialized = PJ_TRUE;
	status = pjsip_endpt_register_module(endpoint, &phase6_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;

	status = pjsip_loop_start(endpoint, &loop);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_loop_start", __LINE__, status);
		goto destroy_endpoint;
	}
	pjsip_transport_add_ref(loop);
	loop_ref_held = PJ_TRUE;
	context.loop = loop;
	if (loop->key.type != PJSIP_TRANSPORT_LOOP_DGRAM ||
	    !(loop->flag & PJSIP_TRANSPORT_DATAGRAM) ||
	    loop->endpt != endpoint ||
	    loop->tpmgr != pjsip_endpt_get_tpmgr(endpoint)) {
		fail_value("loop transport", __LINE__, "loop datagram initialized");
		goto destroy_endpoint;
	}
	status = pjsip_loop_set_recv_delay(loop, 5, NULL);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_loop_set_recv_delay", __LINE__, status);
		goto destroy_endpoint;
	}

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
		fail_status("pj_thread_create event pump", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.event_started, 1,
			   PHASE6_WAIT_MS) != 0) {
		fail_value("event pump", __LINE__, "event thread started");
		goto destroy_endpoint;
	}

	if (test_event_timer(&context) != 0 ||
	    test_successful_transaction(&context) != 0 ||
	    test_retransmit_timeout(&context) != 0 ||
	    test_active_termination(&context) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.callback_error) != 0 ||
	    atomic_get(&context.event_error) != 0) {
		printk("[Phase 6] callback/event error marker=%d status=%d\n",
		       (int)atomic_get(&context.callback_error),
		       (int)atomic_get(&context.event_error));
		goto destroy_endpoint;
	}
	result = 0;

destroy_endpoint:
	if (loop_ref_held) {
		status = pjsip_transport_shutdown(loop);
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_transport_shutdown", __LINE__, status);
			result = -1;
		}
		pjsip_transport_dec_ref(loop);
		loop_ref_held = PJ_FALSE;
		loop = NULL;
		context.loop = NULL;
	}
	if (result == 0 && context.event_thread != NULL &&
	    wait_for_timers(&context, 0, PHASE6_WAIT_MS) != 0) {
		fail_value("loop transport teardown", __LINE__,
			   "event pump drained shutdown timer");
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
	if (module_registered &&
	    (!tsx_layer_initialized || pjsip_tsx_layer_get_tsx_count() == 0)) {
		status = pjsip_endpt_unregister_module(endpoint, &phase6_module);
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
	if (result == 0 &&
	    pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)) != 0) {
		fail_value("endpoint timer teardown", __LINE__, "timer heap empty");
		result = -1;
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
		printk("[Phase 6] lifecycle %d loop/threads/endpoint teardown: PASSED\n",
		       iteration);

destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 6] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase6_loop_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	int iteration;
	int result = 0;

	/* Keep the state-machine coverage fast and deterministic under QEMU. */
	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 320;

	printk("[Phase 6] event/loop/transaction validation (%d lifecycles)\n",
	       PHASE6_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE6_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 6 RESULT: FAILED at lifecycle %d\n", iteration);
			result = 1;
			break;
		}
	}

	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	if (result == 0)
		printk("PHASE 6 RESULT: PASSED (%d/%d lifecycles)\n",
		       PHASE6_LIFECYCLES, PHASE6_LIFECYCLES);
	return result;
}
