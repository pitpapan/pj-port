#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pj/pool_buf.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE11_WAIT_MS 3000
#define PHASE11_CONCURRENT_TRANSACTIONS 8
#define PHASE11_SOAK_MS 30000
#define PHASE11_SOAK_REPORT_MS 5000
#define PHASE11_FIXED_POOL_SIZE 1024

_Static_assert(PJ_HAS_IPV6 == 0,
	       "Phase 11 intentionally validates the IPv4 signaling profile");
_Static_assert(PJSIP_MAX_TRANSPORTS == 16,
	       "Phase 11 capacity report expects 16 endpoint handles");
_Static_assert(PJSIP_MAX_TRANSPORTS <= PJ_IOQUEUE_MAX_HANDLES,
	       "PJSIP transport limit exceeds the PJLIB ioqueue limit");
_Static_assert(PHASE11_CONCURRENT_TRANSACTIONS * 2 < PJSIP_MAX_TSX_COUNT,
	       "Concurrent UAC/UAS transactions need transaction-hash headroom");

int phase7_udp_run(void);
int phase10_signaling_run(void);

struct phase11_context;

struct phase11_options_result {
	struct phase11_context *context;
	atomic_t called;
	atomic_t code;
};

struct phase11_cancel_result {
	struct phase11_context *context;
	pjsip_transaction *transaction;
	atomic_t states;
	atomic_t status;
	atomic_t destroyed;
};

struct phase11_context {
	pjsip_endpoint *endpt;
	pjsip_transport *server_udp;
	pjsip_transport *client_udp;
	pjsip_tpmgr *transport_manager;
	pj_caching_pool *caching_pool;
	pj_thread_t *event_thread;
	pjsip_tp_state_callback previous_state_cb;

	atomic_t event_started;
	atomic_t event_stop;
	atomic_t event_pause;
	atomic_t event_paused;
	atomic_t event_error;
	atomic_t event_polls;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;
	atomic_t options_requests;
	atomic_t dropped_options;
	atomic_t malformed_drops;
	atomic_t boundary_drops;
	atomic_t last_drop_status;
	atomic_t transport_shutdowns;
	atomic_t transport_destroys;
	atomic_t cancelled_transactions;
	atomic_t max_transactions;
	atomic_t max_timers;
	atomic_t max_transports;
	atomic_t event_stack_status;
	atomic_t event_stack_unused;
	atomic_t allocated_pool_bytes;
	atomic_t peak_pool_bytes;
	atomic_t allocated_pool_blocks;
	atomic_t peak_pool_blocks;

	pj_bool_t client_shutdown;
	pj_size_t steady_pool_count;
	pj_size_t steady_pool_size;
	pj_size_t peak_pool_size;
	unsigned soak_rounds;
	unsigned soak_requests;
};

static struct phase11_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 11] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 11] FAIL %s:%d condition=%s\n",
	       test, line, condition);
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

static void record_callback_error(struct phase11_context *context, int error)
{
	atomic_cas(&context->callback_error, 0, error);
}

static void atomic_record_max(atomic_t *maximum, atomic_val_t value)
{
	atomic_val_t previous = atomic_get(maximum);

	while (value > previous && !atomic_cas(maximum, previous, value))
		previous = atomic_get(maximum);
}

static pj_bool_t phase11_on_block_alloc(pj_pool_factory *factory,
					pj_size_t size)
{
	struct phase11_context *context = active_context;
	atomic_val_t bytes;
	atomic_val_t blocks;

	if (context == NULL || &context->caching_pool->factory != factory)
		return PJ_TRUE;
	bytes = atomic_add(&context->allocated_pool_bytes,
			   (atomic_val_t)size) + (atomic_val_t)size;
	blocks = atomic_inc(&context->allocated_pool_blocks) + 1;
	atomic_record_max(&context->peak_pool_bytes, bytes);
	atomic_record_max(&context->peak_pool_blocks, blocks);
	return PJ_TRUE;
}

static void phase11_on_block_free(pj_pool_factory *factory, pj_size_t size)
{
	struct phase11_context *context = active_context;

	if (context == NULL || &context->caching_pool->factory != factory)
		return;
	atomic_sub(&context->allocated_pool_bytes, (atomic_val_t)size);
	atomic_dec(&context->allocated_pool_blocks);
}

static void record_resource_sample(struct phase11_context *context)
{
	unsigned transactions;
	unsigned timers;
	unsigned transports;

	if (context->endpt == NULL)
		return;
	transactions = pjsip_tsx_layer_get_tsx_count();
	timers = (unsigned)pj_timer_heap_count(
		pjsip_endpt_get_timer_heap(context->endpt));
	transports = (context->server_udp != NULL ? 1U : 0U) +
		     (context->client_udp != NULL ? 1U : 0U);
	atomic_record_max(&context->max_transactions, transactions);
	atomic_record_max(&context->max_timers, timers);
	atomic_record_max(&context->max_transports, transports);
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

static unsigned elapsed_msec(const pj_time_val *start)
{
	pj_time_val now;
	pj_time_val elapsed;

	pj_gettimeofday(&now);
	elapsed = now;
	PJ_TIME_VAL_SUB(elapsed, *start);
	return (unsigned)(elapsed.sec * 1000 + elapsed.msec);
}

static int wait_for_count(struct phase11_context *context, atomic_t *value,
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

static int wait_for_transactions(struct phase11_context *context,
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
		record_resource_sample(context);
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_timers(struct phase11_context *context, unsigned expected,
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
		record_resource_sample(context);
		pj_thread_sleep(5);
	}
	return 0;
}

static int phase11_event_thread(void *arg)
{
	struct phase11_context *context = arg;
	size_t unused = 0;
	int stack_status;

	atomic_set(&context->event_started, 1);
	while (!atomic_get(&context->event_stop)) {
		pj_time_val timeout = {0, 10};
		pj_status_t status;

		if (atomic_get(&context->event_pause)) {
			atomic_set(&context->event_paused, 1);
			pj_thread_sleep(1);
			continue;
		}
		atomic_set(&context->event_paused, 0);
		status = pjsip_endpt_handle_events(context->endpt, &timeout);

		atomic_inc(&context->event_polls);
		record_resource_sample(context);
		if (status != PJ_SUCCESS) {
			if (context->client_shutdown &&
			    status == PJ_STATUS_FROM_OS(EBADF)) {
				pj_thread_sleep(1);
				continue;
			}
			atomic_set(&context->event_error, status);
			break;
		}
	}
	stack_status = k_thread_stack_space_get(k_current_get(), &unused);
	atomic_set(&context->event_stack_status, stack_status);
	atomic_set(&context->event_stack_unused, (atomic_val_t)unused);
	return 0;
}

static pj_bool_t request_is_drop_target(const pjsip_rx_data *rdata)
{
	const pjsip_uri *uri = rdata->msg_info.msg->line.req.uri;
	const pjsip_sip_uri *sip_uri;

	if (uri == NULL || (!PJSIP_URI_SCHEME_IS_SIP(uri) &&
			    !PJSIP_URI_SCHEME_IS_SIPS(uri)))
		return PJ_FALSE;
	sip_uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(uri);
	return pj_strcmp2(&sip_uri->user, "drop") == 0;
}

static pj_bool_t phase11_on_rx_request(pjsip_rx_data *rdata);
static void phase11_on_tsx_state(pjsip_transaction *transaction,
				  pjsip_event *event);

static pjsip_module phase11_module = {
	.name = {"phase11-robust", 14},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_rx_request = phase11_on_rx_request,
	.on_tsx_state = phase11_on_tsx_state,
};

static pj_status_t send_local_response(struct phase11_context *context,
				       pjsip_rx_data *rdata, int code)
{
	pjsip_transaction *transaction = NULL;
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_tsx_create_uas(&phase11_module, rdata, &transaction);
	if (status != PJ_SUCCESS)
		return status;
	pjsip_tsx_recv_msg(transaction, rdata);
	status = pjsip_endpt_create_response(context->endpt, rdata, code, NULL,
					    &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_tsx_send_msg(transaction, tdata);
	if (status != PJ_SUCCESS) {
		if (tdata != NULL)
			pjsip_tx_data_dec_ref(tdata);
		pjsip_tsx_terminate(transaction, 500);
	}
	return status;
}

static pj_bool_t phase11_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase11_context *context = active_context;
	pjsip_msg *message = rdata->msg_info.msg;
	pj_status_t status;

	if (context == NULL || message == NULL ||
	    message->type != PJSIP_REQUEST_MSG)
		return PJ_FALSE;
	if (rdata->tp_info.transport != context->server_udp) {
		record_callback_error(context, -501);
		return PJ_TRUE;
	}
	if (message->line.req.method.id != PJSIP_OPTIONS_METHOD)
		return PJ_FALSE;
	if (request_is_drop_target(rdata)) {
		atomic_inc(&context->dropped_options);
		return PJ_TRUE;
	}
	atomic_inc(&context->options_requests);
	status = send_local_response(context, rdata, 200);
	if (status != PJ_SUCCESS)
		record_callback_error(context, status);
	return PJ_TRUE;
}

static void phase11_on_tsx_state(pjsip_transaction *transaction,
				  pjsip_event *event)
{
	struct phase11_cancel_result *result;

	if (phase11_module.id < 0 || event == NULL ||
	    event->type != PJSIP_EVENT_TSX_STATE)
		return;
	result = transaction->mod_data[phase11_module.id];
	if (result == NULL)
		return;
	atomic_or(&result->states, (atomic_val_t)1 << transaction->state);
	atomic_set(&result->status, transaction->status_code);
	if (transaction->state == PJSIP_TSX_STATE_DESTROYED) {
		transaction->mod_data[phase11_module.id] = NULL;
		atomic_set(&result->destroyed, 1);
		atomic_inc(&result->context->cancelled_transactions);
	}
}

static void phase11_options_cb(void *token, pjsip_event *event)
{
	struct phase11_options_result *result = token;
	pjsip_transaction *transaction;

	if (result == NULL || result->context == NULL || event == NULL ||
	    event->type != PJSIP_EVENT_TSX_STATE) {
		if (active_context != NULL)
			record_callback_error(active_context, -502);
		return;
	}
	transaction = event->body.tsx_state.tsx;
	atomic_set(&result->code, transaction->status_code);
	if (atomic_inc(&result->called) != 0)
		record_callback_error(result->context, -503);
}

static pj_status_t create_options_request(struct phase11_context *context,
					  const char *target_user,
					  unsigned sequence,
					  pjsip_tx_data **tdata)
{
	char target_text[96];
	char source_text[96];
	char call_id_text[64];
	pj_str_t target;
	pj_str_t source;
	pj_str_t call_id;
	pjsip_tpselector selector;
	pj_status_t status;

	if (pj_ansi_snprintf(target_text, sizeof(target_text),
			     "sip:%s@127.0.0.1:%u;transport=udp", target_user,
			     context->server_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(source_text, sizeof(source_text),
			     "<sip:phase11@127.0.0.1:%u>",
			     context->client_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(call_id_text, sizeof(call_id_text),
			     "phase11-%u", sequence) <= 0)
		return PJ_ETOOSMALL;
	target = pj_str(target_text);
	source = pj_str(source_text);
	call_id = pj_str(call_id_text);
	status = pjsip_endpt_create_request(
		context->endpt, &pjsip_options_method, &target, &source, &target,
		NULL, &call_id, (int)sequence + 1, NULL, tdata);
	if (status != PJ_SUCCESS)
		return status;
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	status = pjsip_tx_data_set_transport(*tdata, &selector);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(*tdata);
		*tdata = NULL;
	}
	return status;
}

static pj_status_t send_options_request(struct phase11_context *context,
					unsigned sequence,
					struct phase11_options_result *result)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	pj_bzero(result, sizeof(*result));
	result->context = context;
	status = create_options_request(context, "soak", sequence, &tdata);
	if (status != PJ_SUCCESS)
		return status;
	return pjsip_endpt_send_request(context->endpt, tdata, -1, result,
					phase11_options_cb);
}

static int run_concurrent_batch(struct phase11_context *context,
				unsigned first_sequence)
{
	const char *test = "concurrent OPTIONS transactions";
	struct phase11_options_result results[PHASE11_CONCURRENT_TRANSACTIONS];
	atomic_val_t requests_before = atomic_get(&context->options_requests);
	unsigned i;

	for (i = 0; i < PHASE11_CONCURRENT_TRANSACTIONS; ++i) {
		CHECK_STATUS(test, send_options_request(context,
						first_sequence + i, &results[i]));
		record_resource_sample(context);
	}
	for (i = 0; i < PHASE11_CONCURRENT_TRANSACTIONS; ++i) {
		CHECK_TRUE(test, wait_for_count(context, &results[i].called, 1,
					       PHASE11_WAIT_MS) == 0);
		CHECK_TRUE(test, atomic_get(&results[i].code) == 200);
	}
	CHECK_TRUE(test, wait_for_transactions(context, 0,
					      PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_timers(context, 0, PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->options_requests) >=
			 requests_before + PHASE11_CONCURRENT_TRANSACTIONS);
	return 0;
}

static int test_concurrent_transactions(struct phase11_context *context)
{
	if (run_concurrent_batch(context, 1000) != 0)
		return -1;
	printk("[Phase 11] %u simultaneous OPTIONS UAC/UAS exchanges: PASSED\n",
	       PHASE11_CONCURRENT_TRANSACTIONS);
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

static void phase11_on_drop_data(pjsip_tp_dropped_data *data)
{
	struct phase11_context *context = active_context;

	if (context == NULL)
		return;
	if (data->tp != context->server_udp || data->status == PJ_SUCCESS) {
		record_callback_error(context, -504);
		return;
	}
	atomic_set(&context->last_drop_status, data->status);
	if (data->len >= PJSIP_MAX_PKT_LEN)
		atomic_inc(&context->boundary_drops);
	else
		atomic_inc(&context->malformed_drops);
}

static int test_packet_boundaries(struct phase11_context *context)
{
	const char *test = "malformed and boundary-length datagrams";
	static const char malformed[] =
		"OPTIONS sip:broken@127.0.0.1 SIP/2.0\r\n"
		"Broken Header Without Colon\r\n\r\n";
	char below_limit[PJSIP_MAX_PKT_LEN - 1];
	char at_limit[PJSIP_MAX_PKT_LEN];
	char above_limit[PJSIP_MAX_PKT_LEN + 1];
	atomic_val_t malformed_before = atomic_get(&context->malformed_drops);
	atomic_val_t boundary_before = atomic_get(&context->boundary_drops);

	pj_memset(below_limit, 'B', sizeof(below_limit));
	pj_memset(at_limit, 'L', sizeof(at_limit));
	pj_memset(above_limit, 'O', sizeof(above_limit));
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, malformed,
					    sizeof(malformed) - 1));
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, below_limit,
					    sizeof(below_limit)));
	CHECK_TRUE(test, wait_for_count(context, &context->malformed_drops,
					       malformed_before + 2,
					       PHASE11_WAIT_MS) == 0);
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, at_limit,
					    sizeof(at_limit)));
	CHECK_STATUS(test, send_raw_datagram(context->server_udp, above_limit,
					    sizeof(above_limit)));
	CHECK_TRUE(test, wait_for_count(context, &context->boundary_drops,
					       boundary_before + 2,
					       PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->last_drop_status) != PJ_SUCCESS);
	printk("[Phase 11] malformed UDP and packet lengths %u/%u/%u bytes: PASSED\n",
	       (unsigned)sizeof(below_limit), (unsigned)sizeof(at_limit),
	       (unsigned)sizeof(above_limit));
	return 0;
}

static int test_fixed_pool_exhaustion(void)
{
	const char *test = "fixed-pool exhaustion";
	char buffer[PHASE11_FIXED_POOL_SIZE];
	pj_pool_t *pool;
	volatile int caught = 0;
	unsigned i;
	PJ_USE_EXCEPTION;

	pool = pj_pool_create_on_buf("phase11-fixed", buffer, sizeof(buffer));
	CHECK_TRUE(test, pool != NULL);
	PJ_TRY {
		for (i = 0; i < 64; ++i)
			(void)pj_pool_alloc(pool, 128);
	}
	PJ_CATCH_ANY {
		caught = PJ_GET_EXCEPTION();
	}
	PJ_END;
	CHECK_TRUE(test, caught == PJ_NO_MEMORY_EXCEPTION);
	printk("[Phase 11] %u-byte fixed pool exhausted with PJ_NO_MEMORY_EXCEPTION: PASSED\n",
	       PHASE11_FIXED_POOL_SIZE);
	return 0;
}

static int test_event_loop_soak(struct phase11_context *context)
{
	const char *test = "active event-loop soak";
	pj_time_val start;
	unsigned next_report = PHASE11_SOAK_REPORT_MS;
	unsigned sequence = 2000;
	unsigned elapsed;
	atomic_val_t polls_before = atomic_get(&context->event_polls);

	pj_gettimeofday(&start);
	do {
		if (run_concurrent_batch(context, sequence) != 0)
			return -1;
		sequence += PHASE11_CONCURRENT_TRANSACTIONS;
		context->soak_rounds++;
		context->soak_requests += PHASE11_CONCURRENT_TRANSACTIONS;
		record_resource_sample(context);
		if (context->soak_rounds == 1) {
			context->steady_pool_count = context->caching_pool->used_count;
			context->steady_pool_size = (pj_size_t)atomic_get(
				&context->allocated_pool_bytes);
		} else {
			CHECK_TRUE(test, context->caching_pool->used_count ==
					 context->steady_pool_count);
			CHECK_TRUE(test, atomic_get(&context->allocated_pool_bytes) ==
					 context->steady_pool_size);
		}
		elapsed = elapsed_msec(&start);
		if (elapsed >= next_report && elapsed < PHASE11_SOAK_MS) {
			printk("[Phase 11] soak %u ms: rounds=%u requests=%u steady-pool=%u B\n",
			       elapsed, context->soak_rounds, context->soak_requests,
			       (unsigned)context->steady_pool_size);
			next_report += PHASE11_SOAK_REPORT_MS;
		}
	} while (elapsed < PHASE11_SOAK_MS);
	CHECK_TRUE(test, context->soak_rounds > 1);
	CHECK_TRUE(test, atomic_get(&context->event_polls) > polls_before);
	CHECK_TRUE(test, atomic_get(&context->event_error) == 0);
	CHECK_TRUE(test, atomic_get(&context->callback_error) == 0);
	printk("[Phase 11] active QEMU soak %u ms, %u rounds/%u requests, stable pool count/size: PASSED\n",
	       elapsed, context->soak_rounds, context->soak_requests);
	return 0;
}

static pj_status_t start_cancel_transaction(
	struct phase11_context *context, unsigned sequence,
	struct phase11_cancel_result *result)
{
	pjsip_tx_data *tdata = NULL;
	pjsip_transaction *transaction = NULL;
	pjsip_tpselector selector;
	pj_status_t status;

	pj_bzero(result, sizeof(*result));
	result->context = context;
	status = create_options_request(context, "drop", sequence, &tdata);
	if (status != PJ_SUCCESS)
		return status;
	status = pjsip_tsx_create_uac(&phase11_module, tdata, &transaction);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return status;
	}
	result->transaction = transaction;
	transaction->mod_data[phase11_module.id] = result;
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	status = pjsip_tsx_set_transport(transaction, &selector);
	if (status == PJ_SUCCESS)
		status = pjsip_tsx_send_msg(transaction, NULL);
	if (status != PJ_SUCCESS) {
		transaction->mod_data[phase11_module.id] = NULL;
		pjsip_tsx_terminate(transaction, 500);
		pjsip_tx_data_dec_ref(tdata);
		result->transaction = NULL;
	}
	return status;
}

static int test_cancellation_during_shutdown(struct phase11_context *context)
{
	const char *test = "transaction cancellation during transport shutdown";
	struct phase11_cancel_result results[PHASE11_CONCURRENT_TRANSACTIONS];
	atomic_val_t dropped_before = atomic_get(&context->dropped_options);
	unsigned i;

	for (i = 0; i < PHASE11_CONCURRENT_TRANSACTIONS; ++i)
		CHECK_STATUS(test, start_cancel_transaction(context, 9000 + i,
							    &results[i]));
	CHECK_TRUE(test, wait_for_count(context, &context->dropped_options,
					       dropped_before +
					       PHASE11_CONCURRENT_TRANSACTIONS,
					       PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, pjsip_tsx_layer_get_tsx_count() ==
			 PHASE11_CONCURRENT_TRANSACTIONS);
	record_resource_sample(context);
	atomic_set(&context->event_pause, 1);
	CHECK_TRUE(test, wait_for_count(context, &context->event_paused, 1,
					       PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, pjsip_tsx_layer_get_tsx_count() ==
			 PHASE11_CONCURRENT_TRANSACTIONS);
	for (i = 0; i < PHASE11_CONCURRENT_TRANSACTIONS; ++i) {
		CHECK_STATUS(test, pjsip_tsx_terminate(
			results[i].transaction, PJSIP_SC_REQUEST_TERMINATED));
	}
	context->client_shutdown = PJ_TRUE;
	CHECK_STATUS(test, pjsip_transport_shutdown(context->client_udp));
	atomic_set(&context->event_pause, 0);
	for (i = 0; i < PHASE11_CONCURRENT_TRANSACTIONS; ++i)
		CHECK_TRUE(test, wait_for_count(context, &results[i].destroyed, 1,
					       PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0,
					      PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_timers(context, 0, PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_count(context, &context->transport_destroys, 1,
					       PHASE11_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->cancelled_transactions) ==
			 PHASE11_CONCURRENT_TRANSACTIONS);
	context->client_udp = NULL;
	printk("[Phase 11] %u pending transactions cancelled while client UDP shut down; callbacks drained: PASSED\n",
	       PHASE11_CONCURRENT_TRANSACTIONS);
	return 0;
}

static void phase11_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static void phase11_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	struct phase11_context *context = active_context;
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
	if (previous != NULL && previous != phase11_on_transport_state)
		previous(transport, state, info);
}

static int run_phase11_lifecycle(void)
{
	struct phase11_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pjsip_transport *server_udp = NULL;
	pjsip_transport *client_udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t tsx_layer_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t callbacks_installed = PJ_FALSE;
	pj_bool_t server_udp_started = PJ_FALSE;
	pj_bool_t client_udp_started = PJ_FALSE;
	int result = -1;

	pj_bzero(&context, sizeof(context));
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	pj_log_set_level(1);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status("pjlib_util_init", __LINE__, status);
		goto shutdown;
	}
	pj_caching_pool_init(&caching_pool, NULL, 0);
	caching_pool_initialized = PJ_TRUE;
	context.caching_pool = &caching_pool;
	active_context = &context;
	caching_pool.factory.on_block_alloc = phase11_on_block_alloc;
	caching_pool.factory.on_block_free = phase11_on_block_free;
	status = pjsip_endpt_create(&caching_pool.factory, "phase11", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	status = pjsip_endpt_atexit(endpoint, phase11_endpoint_exit);
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
	status = pjsip_endpt_register_module(endpoint, &phase11_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;
	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.transport_manager = transport_manager;
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase11_on_transport_state);
	if (status == PJ_SUCCESS)
		status = pjsip_tpmgr_set_drop_data_cb(transport_manager,
						      phase11_on_drop_data);
	if (status != PJ_SUCCESS) {
		fail_status("transport callbacks", __LINE__, status);
		goto destroy_endpoint;
	}
	callbacks_installed = PJ_TRUE;
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
	thread_pool = pjsip_endpt_create_pool(endpoint, "phase11-thread", 8192,
					      4096);
	if (thread_pool == NULL) {
		fail_value("event thread pool", __LINE__, "thread pool exists");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p11-event", phase11_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("event thread", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.event_started, 1,
			   PHASE11_WAIT_MS) != 0) {
		fail_value("event thread", __LINE__, "event thread started");
		goto destroy_endpoint;
	}
	record_resource_sample(&context);
	printk("[Phase 11] resource endpoint: UDP transports=%u, live ioqueue handles=%u, configured ioqueue limit=%u\n",
	       2U, 2U,
	       (unsigned)PJSIP_MAX_TRANSPORTS);

	if (test_fixed_pool_exhaustion() != 0 ||
	    test_concurrent_transactions(&context) != 0 ||
	    test_packet_boundaries(&context) != 0 ||
	    test_event_loop_soak(&context) != 0 ||
	    test_cancellation_during_shutdown(&context) != 0)
		goto destroy_endpoint;
	client_udp_started = PJ_FALSE;
	client_udp = NULL;
	result = 0;

destroy_endpoint:
	atomic_set(&context.event_pause, 0);
	if (context.event_thread != NULL && tsx_layer_initialized &&
	    (wait_for_transactions(&context, 0, PHASE11_WAIT_MS) != 0 ||
	     wait_for_timers(&context, 0, PHASE11_WAIT_MS) != 0)) {
		fail_value("robustness teardown", __LINE__,
			   "transactions and timers drained");
		result = -1;
	}
	if (client_udp_started) {
		status = pjsip_transport_shutdown(client_udp);
		if (status != PJ_SUCCESS) {
			fail_status("client UDP shutdown", __LINE__, status);
			result = -1;
		}
		client_udp_started = PJ_FALSE;
	}
	if (server_udp_started) {
		status = pjsip_transport_shutdown(server_udp);
		if (status != PJ_SUCCESS) {
			fail_status("server UDP shutdown", __LINE__, status);
			result = -1;
		}
		server_udp_started = PJ_FALSE;
	}
	if (context.event_thread != NULL &&
	    wait_for_count(&context, &context.transport_destroys, 2,
			   PHASE11_WAIT_MS) != 0) {
		fail_value("transport teardown", __LINE__,
			   "two UDP transports destroyed");
		result = -1;
	}
	context.client_udp = NULL;
	context.server_udp = NULL;
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
		status = pjsip_endpt_unregister_module(endpoint, &phase11_module);
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_endpt_unregister_module", __LINE__, status);
			result = -1;
		} else {
			module_registered = PJ_FALSE;
		}
	}
	if (tsx_layer_initialized) {
		status = pjsip_tsx_layer_destroy();
		if (status != PJ_SUCCESS) {
			fail_status("pjsip_tsx_layer_destroy", __LINE__, status);
			result = -1;
		} else {
			tsx_layer_initialized = PJ_FALSE;
		}
	}
	if (thread_pool != NULL) {
		pjsip_endpt_release_pool(endpoint, thread_pool);
		thread_pool = NULL;
	}
	context.peak_pool_size = (pj_size_t)atomic_get(&context.peak_pool_bytes);
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
	if (atomic_get(&context.allocated_pool_bytes) != 0 ||
	    atomic_get(&context.allocated_pool_blocks) != 0) {
		fail_value("PJ heap cleanup", __LINE__,
			   "allocated pool bytes and blocks returned to zero");
		result = -1;
	}
	if (atomic_get(&context.event_stack_status) != 0) {
		fail_value("event stack watermark", __LINE__,
			   "k_thread_stack_space_get succeeded");
		result = -1;
	}
	printk("[Phase 11] resources: PJ heap peak=%u B/%d blocks, steady=%u B/%u pools; max transactions=%d, timers=%d, transports=%d\n",
	       (unsigned)context.peak_pool_size,
	       (int)atomic_get(&context.peak_pool_blocks),
	       (unsigned)context.steady_pool_size,
	       (unsigned)context.steady_pool_count,
	       (int)atomic_get(&context.max_transactions),
	       (int)atomic_get(&context.max_timers),
	       (int)atomic_get(&context.max_transports));
	printk("[Phase 11] event-thread stack: configured=%u B, used<=%u B, unused=%d B\n",
	       (unsigned)CONFIG_DYNAMIC_THREAD_STACK_SIZE,
	       (unsigned)(CONFIG_DYNAMIC_THREAD_STACK_SIZE -
			  atomic_get(&context.event_stack_unused)),
	       (int)atomic_get(&context.event_stack_unused));
	if (result == 0)
		printk("[Phase 11] endpoint/transport/transaction/timer teardown and zero live PJ pools: PASSED\n");

destroy_factory:
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
	active_context = NULL;
shutdown:
	pj_shutdown();
	return result;
}

int phase11_robustness_run(void)
{
	unsigned saved_t1 = pjsip_cfg()->tsx.t1;
	unsigned saved_t2 = pjsip_cfg()->tsx.t2;
	unsigned saved_t4 = pjsip_cfg()->tsx.t4;
	unsigned saved_td = pjsip_cfg()->tsx.td;
	size_t main_unused = 0;
	int main_stack_status;
	int result = 1;

	printk("[Phase 11] robustness/resource validation; Phase 12 remains disabled\n");
	if (phase7_udp_run() != 0) {
		printk("PHASE 11 RESULT: FAILED (Phase 7 regression)\n");
		return 1;
	}
	if (phase10_signaling_run() != 0) {
		printk("PHASE 11 RESULT: FAILED (Phase 10 regression)\n");
		return 1;
	}
	pjsip_cfg()->tsx.t1 = 40;
	pjsip_cfg()->tsx.t2 = 80;
	pjsip_cfg()->tsx.t4 = 100;
	pjsip_cfg()->tsx.td = 1000;
	if (run_phase11_lifecycle() == 0)
		result = 0;
	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	main_stack_status = k_thread_stack_space_get(k_current_get(), &main_unused);
	if (main_stack_status != 0) {
		printk("[Phase 11] FAIL main stack watermark status=%d\n",
		       main_stack_status);
		result = 1;
	} else {
		printk("[Phase 11] main-thread stack: configured=%u B, used<=%u B, unused=%u B\n",
		       (unsigned)CONFIG_MAIN_STACK_SIZE,
		       (unsigned)(CONFIG_MAIN_STACK_SIZE - main_unused),
		       (unsigned)main_unused);
	}
	if (result == 0)
		printk("PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)\n");
	else
		printk("PHASE 11 RESULT: FAILED\n");
	return result;
}
