#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <errno.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE9_LIFECYCLES 2
#define PHASE9_WAIT_MS 2500
#define PHASE9_DNS_PACKET_SIZE 768

#define DNS_HEADER_SIZE 12
#define DNS_CLASS_IN 1
#define DNS_FLAGS_RESPONSE_OK 0x8180
#define DNS_FLAGS_NXDOMAIN 0x8183
#define DNS_FLAGS_NOTIMPL 0x8184

#define STATE_BIT(state) ((atomic_val_t)1 << (state))

_Static_assert(PJSIP_HAS_RESOLVER == 1,
	       "Phase 9 requires the PJSIP asynchronous resolver");
_Static_assert(PJ_HAS_IPV6 == 0,
	       "Phase 9 intentionally validates IPv4 resolution only");

struct phase9_resolution_result {
	atomic_t called;
	atomic_t status;
	pjsip_server_addresses addresses;
};

struct phase9_dns_result {
	atomic_t called;
	atomic_t status;
};

struct phase9_context {
	pjsip_endpoint *endpt;
	pj_dns_resolver *resolver;
	pjsip_transport *server_udp;
	pjsip_transport *client_udp;
	pj_sock_t dns_socket;
	pj_uint16_t dns_port;
	pj_uint16_t sip_port;
	pj_uint16_t backup_port;
	pj_thread_t *event_thread;
	pj_thread_t *dns_thread;
	pjsip_tp_state_callback previous_state_cb;

	atomic_t event_stop;
	atomic_t event_started;
	atomic_t event_polls;
	atomic_t event_error;
	atomic_t dns_stop;
	atomic_t dns_started;
	atomic_t dns_error;
	atomic_t teardown_started;
	atomic_t teardown_poll_retries;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;

	atomic_t dns_a_queries;
	atomic_t dns_srv_queries;
	atomic_t dns_primary_queries;
	atomic_t dns_backup_queries;
	atomic_t dns_missing_queries;
	atomic_t dns_timeout_queries;
	atomic_t dns_malformed_queries;
	atomic_t dns_cancel_queries;
	atomic_t dns_aaaa_queries;
	atomic_t dns_naptr_queries;
	atomic_t dns_unexpected_queries;
	atomic_t dns_replies;

	atomic_t sip_requests;
	atomic_t sip_source_port;
	atomic_t uac_states;
	atomic_t uas_states;
	atomic_t uac_status;
	atomic_t uas_status;
	atomic_t uac_retransmits;
	atomic_t transport_shutdowns;
	atomic_t transport_destroys;
};

static struct phase9_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 9] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 9] FAIL %s:%d condition=%s\n", test, line, condition);
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

static void record_callback_error(struct phase9_context *context, int error)
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

static int wait_for_count(struct phase9_context *context, atomic_t *value,
			  atomic_val_t minimum, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (atomic_get(value) < minimum) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0 ||
		    atomic_get(&context->dns_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_bits(struct phase9_context *context, atomic_t *value,
			 atomic_val_t expected, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while ((atomic_get(value) & expected) != expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0 ||
		    atomic_get(&context->dns_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_transactions(struct phase9_context *context,
				 unsigned expected, unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (pjsip_tsx_layer_get_tsx_count() != expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0 ||
		    atomic_get(&context->dns_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static int wait_for_timers(struct phase9_context *context, unsigned expected,
			   unsigned timeout_ms)
{
	pj_time_val deadline;

	make_deadline(&deadline, timeout_ms);
	while (pj_timer_heap_count(pjsip_endpt_get_timer_heap(context->endpt)) !=
	       expected) {
		if (atomic_get(&context->callback_error) != 0 ||
		    atomic_get(&context->event_error) != 0 ||
		    atomic_get(&context->dns_error) != 0)
			return -2;
		if (deadline_reached(&deadline))
			return -1;
		pj_thread_sleep(5);
	}
	return 0;
}

static pj_uint16_t read_u16(const pj_uint8_t *data)
{
	return (pj_uint16_t)(((pj_uint16_t)data[0] << 8) | data[1]);
}

static void write_u16(pj_uint8_t *data, pj_uint16_t value)
{
	data[0] = (pj_uint8_t)(value >> 8);
	data[1] = (pj_uint8_t)value;
}

static void write_u32(pj_uint8_t *data, pj_uint32_t value)
{
	data[0] = (pj_uint8_t)(value >> 24);
	data[1] = (pj_uint8_t)(value >> 16);
	data[2] = (pj_uint8_t)(value >> 8);
	data[3] = (pj_uint8_t)value;
}

static int append_u16(pj_uint8_t *packet, pj_size_t capacity,
		      pj_size_t *offset, pj_uint16_t value)
{
	if (*offset + 2 > capacity)
		return -1;
	write_u16(packet + *offset, value);
	*offset += 2;
	return 0;
}

static int append_u32(pj_uint8_t *packet, pj_size_t capacity,
		      pj_size_t *offset, pj_uint32_t value)
{
	if (*offset + 4 > capacity)
		return -1;
	write_u32(packet + *offset, value);
	*offset += 4;
	return 0;
}

static int append_dns_name(pj_uint8_t *packet, pj_size_t capacity,
			   pj_size_t *offset, const char *name)
{
	const char *label = name;
	const char *cursor = name;

	while (1) {
		pj_size_t length;

		while (*cursor != '\0' && *cursor != '.')
			++cursor;
		length = (pj_size_t)(cursor - label);
		if (length > 63 || *offset + 1 + length > capacity)
			return -1;
		packet[(*offset)++] = (pj_uint8_t)length;
		if (length != 0) {
			pj_memcpy(packet + *offset, label, length);
			*offset += length;
		}
		if (*cursor == '\0')
			break;
		++cursor;
		label = cursor;
	}
	if (*offset + 1 > capacity)
		return -1;
	packet[(*offset)++] = 0;
	return 0;
}

static int parse_dns_question(const pj_uint8_t *packet, pj_size_t size,
			      char *name, pj_size_t name_size,
			      pj_uint16_t *type, pj_size_t *question_end)
{
	pj_size_t offset = DNS_HEADER_SIZE;
	pj_size_t used = 0;

	if (size < DNS_HEADER_SIZE || read_u16(packet + 4) != 1)
		return -1;
	while (offset < size) {
		unsigned length = packet[offset++];

		if (length == 0)
			break;
		if ((length & 0xc0) != 0 || offset + length > size)
			return -1;
		if (used != 0) {
			if (used + 1 >= name_size)
				return -1;
			name[used++] = '.';
		}
		if (used + length >= name_size)
			return -1;
		pj_memcpy(name + used, packet + offset, length);
		used += length;
		offset += length;
	}
	if (offset + 4 > size || used == 0 || read_u16(packet + offset + 2) !=
	    DNS_CLASS_IN)
		return -1;
	name[used] = '\0';
	*type = read_u16(packet + offset);
	*question_end = offset + 4;
	return 0;
}

static int append_a_record(pj_uint8_t *packet, pj_size_t capacity,
			   pj_size_t *offset, const char *owner,
			   const pj_uint8_t address[4], pj_bool_t question_owner)
{
	if (question_owner) {
		if (append_u16(packet, capacity, offset, 0xc00c) != 0)
			return -1;
	} else if (append_dns_name(packet, capacity, offset, owner) != 0) {
		return -1;
	}
	if (append_u16(packet, capacity, offset, PJ_DNS_TYPE_A) != 0 ||
	    append_u16(packet, capacity, offset, DNS_CLASS_IN) != 0 ||
	    append_u32(packet, capacity, offset, 60) != 0 ||
	    append_u16(packet, capacity, offset, 4) != 0 ||
	    *offset + 4 > capacity)
		return -1;
	pj_memcpy(packet + *offset, address, 4);
	*offset += 4;
	return 0;
}

static int append_srv_record(pj_uint8_t *packet, pj_size_t capacity,
			     pj_size_t *offset, pj_uint16_t priority,
			     pj_uint16_t port, const char *target)
{
	pj_size_t length_offset;
	pj_size_t rdata_start;

	if (append_u16(packet, capacity, offset, 0xc00c) != 0 ||
	    append_u16(packet, capacity, offset, PJ_DNS_TYPE_SRV) != 0 ||
	    append_u16(packet, capacity, offset, DNS_CLASS_IN) != 0 ||
	    append_u32(packet, capacity, offset, 60) != 0)
		return -1;
	length_offset = *offset;
	if (append_u16(packet, capacity, offset, 0) != 0)
		return -1;
	rdata_start = *offset;
	if (append_u16(packet, capacity, offset, priority) != 0 ||
	    append_u16(packet, capacity, offset, 0) != 0 ||
	    append_u16(packet, capacity, offset, port) != 0 ||
	    append_dns_name(packet, capacity, offset, target) != 0)
		return -1;
	write_u16(packet + length_offset,
		  (pj_uint16_t)(*offset - rdata_start));
	return 0;
}

static int start_dns_response(pj_uint8_t *response, pj_size_t capacity,
			      const pj_uint8_t *query, pj_size_t question_end,
			      pj_uint16_t flags, pj_uint16_t answers,
			      pj_uint16_t additional, pj_size_t *offset)
{
	if (question_end < DNS_HEADER_SIZE || question_end > capacity)
		return -1;
	pj_bzero(response, DNS_HEADER_SIZE);
	write_u16(response, read_u16(query));
	write_u16(response + 2, flags);
	write_u16(response + 4, 1);
	write_u16(response + 6, answers);
	write_u16(response + 10, additional);
	pj_memcpy(response + DNS_HEADER_SIZE, query + DNS_HEADER_SIZE,
		  question_end - DNS_HEADER_SIZE);
	*offset = question_end;
	return 0;
}

static int build_dns_response(struct phase9_context *context,
			      const pj_uint8_t *query, pj_size_t question_end,
			      const char *name, pj_uint16_t type,
			      pj_uint8_t *response, pj_size_t capacity)
{
	static const pj_uint8_t a_test[4] = {127, 0, 0, 42};
	static const pj_uint8_t primary[4] = {127, 0, 0, 1};
	static const pj_uint8_t backup[4] = {127, 0, 0, 2};
	pj_size_t offset;

	if (pj_ansi_strcmp(name, "a.phase9.test") == 0 &&
	    type == PJ_DNS_TYPE_A) {
		if (start_dns_response(response, capacity, query, question_end,
				       DNS_FLAGS_RESPONSE_OK, 1, 0, &offset) != 0 ||
		    append_a_record(response, capacity, &offset, NULL, a_test,
				    PJ_TRUE) != 0)
			return -1;
		return (int)offset;
	}
	if (pj_ansi_strcmp(name, "_sip._udp.srv.phase9.test") == 0 &&
	    type == PJ_DNS_TYPE_SRV) {
		if (start_dns_response(response, capacity, query, question_end,
				       DNS_FLAGS_RESPONSE_OK, 2, 2, &offset) != 0 ||
		    append_srv_record(response, capacity, &offset, 10,
				      context->sip_port,
				      "primary.phase9.test") != 0 ||
		    append_srv_record(response, capacity, &offset, 20,
				      context->backup_port,
				      "backup.phase9.test") != 0 ||
		    append_a_record(response, capacity, &offset,
				    "primary.phase9.test", primary, PJ_FALSE) != 0 ||
		    append_a_record(response, capacity, &offset,
				    "backup.phase9.test", backup, PJ_FALSE) != 0)
			return -1;
		return (int)offset;
	}
	if (pj_ansi_strcmp(name, "primary.phase9.test") == 0 &&
	    type == PJ_DNS_TYPE_A) {
		if (start_dns_response(response, capacity, query, question_end,
				       DNS_FLAGS_RESPONSE_OK, 1, 0, &offset) != 0 ||
		    append_a_record(response, capacity, &offset, NULL, primary,
				    PJ_TRUE) != 0)
			return -1;
		return (int)offset;
	}
	if (pj_ansi_strcmp(name, "backup.phase9.test") == 0 &&
	    type == PJ_DNS_TYPE_A) {
		if (start_dns_response(response, capacity, query, question_end,
				       DNS_FLAGS_RESPONSE_OK, 1, 0, &offset) != 0 ||
		    append_a_record(response, capacity, &offset, NULL, backup,
				    PJ_TRUE) != 0)
			return -1;
		return (int)offset;
	}
	if (pj_ansi_strcmp(name, "missing.phase9.test") == 0) {
		if (start_dns_response(response, capacity, query, question_end,
				       DNS_FLAGS_NXDOMAIN, 0, 0, &offset) != 0)
			return -1;
		return (int)offset;
	}
	if (start_dns_response(response, capacity, query, question_end,
			       DNS_FLAGS_NOTIMPL, 0, 0, &offset) != 0)
		return -1;
	return (int)offset;
}

static void count_dns_query(struct phase9_context *context, const char *name,
			    pj_uint16_t type)
{
	if (type == PJ_DNS_TYPE_AAAA) {
		atomic_inc(&context->dns_aaaa_queries);
		return;
	}
	if (type == PJ_DNS_TYPE_NAPTR) {
		atomic_inc(&context->dns_naptr_queries);
		return;
	}
	if (pj_ansi_strcmp(name, "a.phase9.test") == 0)
		atomic_inc(&context->dns_a_queries);
	else if (pj_ansi_strcmp(name, "_sip._udp.srv.phase9.test") == 0)
		atomic_inc(&context->dns_srv_queries);
	else if (pj_ansi_strcmp(name, "primary.phase9.test") == 0)
		atomic_inc(&context->dns_primary_queries);
	else if (pj_ansi_strcmp(name, "backup.phase9.test") == 0)
		atomic_inc(&context->dns_backup_queries);
	else if (pj_ansi_strcmp(name, "missing.phase9.test") == 0)
		atomic_inc(&context->dns_missing_queries);
	else if (pj_ansi_strcmp(name, "timeout.phase9.test") == 0)
		atomic_inc(&context->dns_timeout_queries);
	else if (pj_ansi_strcmp(name, "malformed.phase9.test") == 0)
		atomic_inc(&context->dns_malformed_queries);
	else if (pj_ansi_strcmp(name, "cancel.phase9.test") == 0)
		atomic_inc(&context->dns_cancel_queries);
	else
		atomic_inc(&context->dns_unexpected_queries);
}

static int phase9_dns_thread(void *arg)
{
	struct phase9_context *context = arg;
	pj_uint8_t query[PHASE9_DNS_PACKET_SIZE];
	pj_uint8_t response[PHASE9_DNS_PACKET_SIZE];

	atomic_set(&context->dns_started, 1);
	while (!atomic_get(&context->dns_stop)) {
		pj_fd_set_t read_set;
		pj_time_val delay = {0, 20};
		pj_sockaddr source;
		int source_length = sizeof(source);
		pj_ssize_t length = sizeof(query);
		pj_ssize_t sent;
		char name[256];
		pj_uint16_t type;
		pj_size_t question_end;
		int ready;
		int response_length;
		pj_status_t status;

		PJ_FD_ZERO(&read_set);
		PJ_FD_SET(context->dns_socket, &read_set);
		ready = pj_sock_select((int)context->dns_socket + 1, &read_set,
				       NULL, NULL, &delay);
		if (ready < 0) {
			atomic_set(&context->dns_error, pj_get_netos_error());
			break;
		}
		if (ready == 0)
			continue;
		status = pj_sock_recvfrom(context->dns_socket, query, &length, 0,
					  &source, &source_length);
		if (status != PJ_SUCCESS) {
			atomic_set(&context->dns_error, status);
			break;
		}
		if (parse_dns_question(query, (pj_size_t)length, name,
				       sizeof(name), &type, &question_end) != 0) {
			atomic_inc(&context->dns_unexpected_queries);
			continue;
		}
		count_dns_query(context, name, type);
		if (pj_ansi_strcmp(name, "timeout.phase9.test") == 0 ||
		    pj_ansi_strcmp(name, "cancel.phase9.test") == 0)
			continue;
		if (pj_ansi_strcmp(name, "malformed.phase9.test") == 0) {
			response[0] = query[0];
			response[1] = query[1];
			response[2] = 0x80;
			response_length = 3;
		} else {
			response_length = build_dns_response(
				context, query, question_end, name, type, response,
				sizeof(response));
			if (response_length < 0) {
				record_callback_error(context, -301);
				continue;
			}
		}
		sent = response_length;
		status = pj_sock_sendto(context->dns_socket, response, &sent, 0,
					&source, source_length);
		if (status != PJ_SUCCESS || sent != response_length) {
			atomic_set(&context->dns_error,
				   status != PJ_SUCCESS ? status : PJ_EUNKNOWN);
			break;
		}
		atomic_inc(&context->dns_replies);
	}
	return 0;
}

static int phase9_event_thread(void *arg)
{
	struct phase9_context *context = arg;

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

static void phase9_resolution_cb(pj_status_t status, void *token,
				 const pjsip_server_addresses *addresses)
{
	struct phase9_resolution_result *result = token;
	struct phase9_context *context = active_context;

	if (!atomic_cas(&result->called, 0, -1)) {
		if (context != NULL)
			record_callback_error(context, -310);
		return;
	}
	atomic_set(&result->status, status);
	if (addresses != NULL)
		result->addresses = *addresses;
	atomic_set(&result->called, 1);
}

static void phase9_dns_cb(void *user_data, pj_status_t status,
			  pj_dns_parsed_packet *response)
{
	struct phase9_dns_result *result = user_data;
	struct phase9_context *context = active_context;

	PJ_UNUSED_ARG(response);
	if (!atomic_cas(&result->called, 0, -1)) {
		if (context != NULL)
			record_callback_error(context, -311);
		return;
	}
	atomic_set(&result->status, status);
	atomic_set(&result->called, 1);
}

static int resolve_target(struct phase9_context *context, pj_pool_t *pool,
			  const char *host, int port,
			  struct phase9_resolution_result *result)
{
	pjsip_host_info target;

	pj_bzero(result, sizeof(*result));
	pj_bzero(&target, sizeof(target));
	target.type = PJSIP_TRANSPORT_UDP;
	target.flag = pjsip_transport_get_flag_from_type(target.type);
	target.addr.host = pj_str((char *)host);
	target.addr.port = port;
	pjsip_endpt_resolve(context->endpt, pool, &target, result,
			    phase9_resolution_cb);
	return wait_for_count(context, &result->called, 1, PHASE9_WAIT_MS);
}

static int address_equals(const pj_sockaddr *address, const char *expected)
{
	char text[PJ_INET_ADDRSTRLEN];

	pj_sockaddr_print(address, text, sizeof(text), 0);
	return pj_ansi_strcmp(text, expected) == 0;
}

static int test_a_lookup(struct phase9_context *context, pj_pool_t *pool)
{
	const char *test = "PJSIP A lookup";
	struct phase9_resolution_result result;

	CHECK_TRUE(test, resolve_target(context, pool, "a.phase9.test", 5088,
					&result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, result.addresses.count == 1);
	CHECK_TRUE(test, address_equals(&result.addresses.entry[0].addr,
					"127.0.0.42"));
	CHECK_TRUE(test, pj_sockaddr_get_port(&result.addresses.entry[0].addr) ==
			 5088);
	CHECK_TRUE(test, result.addresses.entry[0].type == PJSIP_TRANSPORT_UDP);
	CHECK_TRUE(test, atomic_get(&context->dns_a_queries) == 1);
	printk("[Phase 9] asynchronous A lookup a.phase9.test -> 127.0.0.42:5088: PASSED\n");
	return 0;
}

static int test_srv_lookup(struct phase9_context *context, pj_pool_t *pool)
{
	const char *test = "PJSIP SIP SRV lookup";
	struct phase9_resolution_result result;

	CHECK_TRUE(test, resolve_target(context, pool, "srv.phase9.test", 0,
					&result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, result.addresses.count == 2);
	CHECK_TRUE(test, result.addresses.entry[0].priority == 10);
	CHECK_TRUE(test, result.addresses.entry[1].priority == 20);
	CHECK_TRUE(test, pj_sockaddr_get_port(&result.addresses.entry[0].addr) ==
			 context->sip_port);
	CHECK_TRUE(test, pj_sockaddr_get_port(&result.addresses.entry[1].addr) ==
			 context->backup_port);
	CHECK_TRUE(test, address_equals(&result.addresses.entry[0].addr,
					"127.0.0.1"));
	CHECK_TRUE(test, address_equals(&result.addresses.entry[1].addr,
					"127.0.0.2"));
	CHECK_TRUE(test, atomic_get(&context->dns_srv_queries) == 1);
	CHECK_TRUE(test, atomic_get(&context->dns_aaaa_queries) == 0);
	CHECK_TRUE(test, atomic_get(&context->dns_naptr_queries) == 0);
	printk("[Phase 9] _sip._udp SRV priority ordering and selected ports %u/%u: PASSED\n",
	       context->sip_port, context->backup_port);
	return 0;
}

static pj_status_t send_transaction_response(struct phase9_context *context,
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

static pj_bool_t phase9_on_rx_request(pjsip_rx_data *rdata);
static void phase9_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event);

static pjsip_module phase9_module = {
	.name = {"phase9-validation", 17},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_UA_PROXY_LAYER - 1,
	.on_rx_request = phase9_on_rx_request,
	.on_tsx_state = phase9_on_tsx_state,
};

static pj_bool_t phase9_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase9_context *context = active_context;
	pjsip_transaction *uas;
	pj_status_t status;

	if (context == NULL || rdata->msg_info.cid == NULL ||
	    pj_strcmp2(&rdata->msg_info.cid->id, "phase9-transport") != 0)
		return PJ_FALSE;
	atomic_inc(&context->sip_requests);
	atomic_set(&context->sip_source_port, rdata->pkt_info.src_port);
	if (rdata->tp_info.transport != context->server_udp ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_OPTIONS_METHOD) {
		record_callback_error(context, -320);
		return PJ_TRUE;
	}
	status = pjsip_tsx_create_uas(&phase9_module, rdata, &uas);
	if (status != PJ_SUCCESS) {
		record_callback_error(context, -321);
		return PJ_TRUE;
	}
	uas->mod_data[phase9_module.id] = (void *)(uintptr_t)1;
	pjsip_tsx_recv_msg(uas, rdata);
	status = send_transaction_response(context, uas, rdata, 200);
	if (status != PJ_SUCCESS)
		record_callback_error(context, -322);
	return PJ_TRUE;
}

static void phase9_on_tsx_state(pjsip_transaction *tsx, pjsip_event *event)
{
	struct phase9_context *context = active_context;

	if (context == NULL || phase9_module.id < 0 ||
	    (uintptr_t)tsx->mod_data[phase9_module.id] != 1) {
		if (context != NULL)
			record_callback_error(context, -330);
		return;
	}
	if (event->type == PJSIP_EVENT_TSX_STATE &&
	    event->body.tsx_state.type == PJSIP_EVENT_RX_MSG) {
		pjsip_transport *expected = tsx->role == PJSIP_ROLE_UAC ?
			context->client_udp : context->server_udp;

		if (event->body.tsx_state.src.rdata->tp_info.transport != expected)
			record_callback_error(context, -331);
	}
	if (tsx->role == PJSIP_ROLE_UAC) {
		atomic_or(&context->uac_states, STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uac_status, tsx->status_code);
		if (tsx->retransmit_count > atomic_get(&context->uac_retransmits))
			atomic_set(&context->uac_retransmits,
				   tsx->retransmit_count);
	} else {
		atomic_or(&context->uas_states, STATE_BIT(tsx->state));
		if (tsx->state == PJSIP_TSX_STATE_COMPLETED ||
		    tsx->state == PJSIP_TSX_STATE_TERMINATED)
			atomic_set(&context->uas_status, tsx->status_code);
	}
}

static int start_resolved_transaction(struct phase9_context *context)
{
	const char *test = "start resolved UDP OPTIONS";
	char from_text[96];
	pj_str_t target = pj_str("sip:service@srv.phase9.test;transport=udp");
	pj_str_t from;
	pj_str_t call_id = pj_str("phase9-transport");
	pjsip_tpselector selector;
	pjsip_tx_data *tdata = NULL;
	pjsip_via_hdr *via;
	pjsip_transaction *tsx = NULL;
	pj_status_t status;
	int length;

	length = pj_ansi_snprintf(from_text, sizeof(from_text),
				  "<sip:phase9@127.0.0.1:%d>",
				  context->client_udp->local_name.port);
	if (length <= 0 || length >= (int)sizeof(from_text))
		return fail_value(test, __LINE__, "From URI fits");
	from = pj_str(from_text);
	status = pjsip_endpt_create_request(context->endpt,
					    &pjsip_options_method, &target,
					    &from, &target, NULL, &call_id,
					    900, NULL, &tdata);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	via = (pjsip_via_hdr *)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA,
						  NULL);
	if (via == NULL) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_value(test, __LINE__, "Via header exists");
	}
	pj_strdup2(tdata->pool, &via->branch_param,
		   PJSIP_RFC3261_BRANCH_ID "-phase9-transport");
	status = pjsip_tsx_create_uac(&phase9_module, tdata, &tsx);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	tsx->mod_data[phase9_module.id] = (void *)(uintptr_t)1;
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

static int test_resolved_transport(struct phase9_context *context)
{
	const char *test = "resolved UDP transport routing";
	atomic_val_t uac_expected = STATE_BIT(PJSIP_TSX_STATE_CALLING) |
		STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);
	atomic_val_t uas_expected = STATE_BIT(PJSIP_TSX_STATE_TRYING) |
		STATE_BIT(PJSIP_TSX_STATE_COMPLETED) |
		STATE_BIT(PJSIP_TSX_STATE_TERMINATED) |
		STATE_BIT(PJSIP_TSX_STATE_DESTROYED);

	CHECK_TRUE(test, start_resolved_transaction(context) == 0);
	CHECK_TRUE(test, wait_for_bits(context, &context->uac_states,
				       uac_expected, PHASE9_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_bits(context, &context->uas_states,
				       uas_expected, PHASE9_WAIT_MS) == 0);
	CHECK_TRUE(test, wait_for_transactions(context, 0, PHASE9_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->sip_requests) == 1);
	CHECK_TRUE(test, atomic_get(&context->sip_source_port) ==
			 context->client_udp->local_name.port);
	CHECK_TRUE(test, atomic_get(&context->uac_status) == 200);
	CHECK_TRUE(test, atomic_get(&context->uas_status) == 200);
	CHECK_TRUE(test, atomic_get(&context->uac_retransmits) == 0);
	CHECK_TRUE(test, atomic_get(&context->dns_srv_queries) == 1);
	CHECK_TRUE(test, atomic_get(&context->dns_primary_queries) == 1);
	CHECK_TRUE(test, atomic_get(&context->dns_backup_queries) == 1);
	printk("[Phase 9] cached SRV plus A resolution routed OPTIONS to UDP port %u: PASSED\n",
	       context->sip_port);
	return 0;
}

static int test_numeric_fallback(struct phase9_context *context,
				 pj_pool_t *pool)
{
	const char *test = "numeric address fallback";
	struct phase9_resolution_result result;
	atomic_val_t queries = atomic_get(&context->dns_replies);

	CHECK_TRUE(test, resolve_target(context, pool, "127.0.0.9", 5077,
					&result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, result.addresses.count == 1);
	CHECK_TRUE(test, address_equals(&result.addresses.entry[0].addr,
					"127.0.0.9"));
	CHECK_TRUE(test, pj_sockaddr_get_port(&result.addresses.entry[0].addr) ==
			 5077);
	CHECK_TRUE(test, atomic_get(&context->dns_replies) == queries);
	printk("[Phase 9] numeric IPv4 fallback bypassed DNS and preserved port: PASSED\n");
	return 0;
}

static int test_missing(struct phase9_context *context, pj_pool_t *pool)
{
	const char *test = "DNS missing record";
	struct phase9_resolution_result result;

	CHECK_TRUE(test, resolve_target(context, pool, "missing.phase9.test",
					5070, &result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) ==
			 PJLIB_UTIL_EDNS_NXDOMAIN);
	CHECK_TRUE(test, result.addresses.count == 0);
	CHECK_TRUE(test, atomic_get(&context->dns_missing_queries) == 1);
	printk("[Phase 9] NXDOMAIN propagated through PJSIP resolver: PASSED\n");
	return 0;
}

static int test_timeout(struct phase9_context *context, pj_pool_t *pool)
{
	const char *test = "DNS timeout and retransmission";
	struct phase9_resolution_result result;

	CHECK_TRUE(test, resolve_target(context, pool, "timeout.phase9.test",
					5070, &result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_ETIMEDOUT);
	CHECK_TRUE(test, atomic_get(&context->dns_timeout_queries) == 3);
	printk("[Phase 9] silent DNS target retried three times then timed out: PASSED\n");
	return 0;
}

static int test_cancel(struct phase9_context *context)
{
	const char *test = "DNS asynchronous cancellation";
	struct phase9_dns_result result;
	pj_dns_async_query *query = NULL;
	pj_str_t name = pj_str("cancel.phase9.test");
	pj_status_t status;
	atomic_val_t count;

	pj_bzero(&result, sizeof(result));
	status = pj_dns_resolver_start_query(context->resolver, &name,
					     PJ_DNS_TYPE_A, 0,
					     phase9_dns_cb, &result, &query);
	CHECK_STATUS(test, status);
	CHECK_TRUE(test, query != NULL);
	CHECK_TRUE(test, wait_for_count(context, &context->dns_cancel_queries, 1,
				       PHASE9_WAIT_MS) == 0);
	CHECK_STATUS(test, pj_dns_resolver_cancel_query(query, PJ_TRUE));
	CHECK_TRUE(test, wait_for_count(context, &result.called, 1,
				       PHASE9_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_ECANCELLED);
	count = atomic_get(&context->dns_cancel_queries);
	pj_thread_sleep(200);
	CHECK_TRUE(test, atomic_get(&context->dns_cancel_queries) == count);
	CHECK_TRUE(test, atomic_get(&result.called) == 1);
	printk("[Phase 9] pending DNS query cancellation notified once and stopped retransmission: PASSED\n");
	return 0;
}

static int test_malformed(struct phase9_context *context, pj_pool_t *pool)
{
	const char *test = "malformed DNS responses";
	struct phase9_resolution_result result;

	CHECK_TRUE(test, resolve_target(context, pool, "malformed.phase9.test",
					5070, &result) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_ETIMEDOUT);
	CHECK_TRUE(test, atomic_get(&context->dns_malformed_queries) == 3);
	printk("[Phase 9] malformed DNS replies were rejected, retried, and timed out safely: PASSED\n");
	return 0;
}

static void phase9_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static void phase9_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	struct phase9_context *context = active_context;
	pjsip_tp_state_callback previous = NULL;

	PJ_UNUSED_ARG(info);
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
	if (previous != NULL && previous != phase9_on_transport_state)
		previous(transport, state, info);
}

static int run_lifecycle(int iteration)
{
	struct phase9_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pj_pool_t *test_pool = NULL;
	pjsip_transport *server_udp = NULL;
	pjsip_transport *client_udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pj_dns_resolver *resolver = NULL;
	pj_sockaddr_in bind_address;
	pj_sockaddr_in dns_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_str_t nameserver = pj_str("127.0.0.1");
	pj_dns_settings dns_settings;
	pj_uint16_t nameserver_port;
	pj_status_t status;
	pj_bool_t caching_pool_initialized = PJ_FALSE;
	pj_bool_t tsx_layer_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	pj_bool_t callbacks_installed = PJ_FALSE;
	pj_bool_t resolver_created = PJ_FALSE;
	pj_bool_t server_udp_started = PJ_FALSE;
	pj_bool_t client_udp_started = PJ_FALSE;
	int dns_address_length = sizeof(dns_address);
	int result = -1;

	pj_bzero(&context, sizeof(context));
	context.dns_socket = PJ_INVALID_SOCKET;
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
	status = pjsip_endpt_create(&caching_pool.factory, "phase9", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
	status = pjsip_endpt_atexit(endpoint, phase9_endpoint_exit);
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
	status = pjsip_endpt_register_module(endpoint, &phase9_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;
	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase9_on_transport_state);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_state_cb", __LINE__, status);
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
	context.sip_port = (pj_uint16_t)server_udp->local_name.port;
	context.backup_port = context.sip_port == 5099 ? 5100 : 5099;
	status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
					   &client_udp);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_udp_transport_start client", __LINE__, status);
		goto destroy_endpoint;
	}
	client_udp_started = PJ_TRUE;
	context.client_udp = client_udp;

	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM(), 0,
				&context.dns_socket);
	if (status != PJ_SUCCESS) {
		fail_status("DNS responder socket", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pj_sockaddr_in_init(&dns_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pj_sock_bind(context.dns_socket, &dns_address,
				      sizeof(dns_address));
	if (status == PJ_SUCCESS)
		status = pj_sock_getsockname(context.dns_socket, &dns_address,
					     &dns_address_length);
	if (status != PJ_SUCCESS) {
		fail_status("DNS responder bind", __LINE__, status);
		goto destroy_endpoint;
	}
	context.dns_port = (pj_uint16_t)pj_sockaddr_get_port(&dns_address);

	status = pjsip_endpt_create_resolver(endpoint, &resolver);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create_resolver", __LINE__, status);
		goto destroy_endpoint;
	}
	resolver_created = PJ_TRUE;
	context.resolver = resolver;
	nameserver_port = context.dns_port;
	status = pj_dns_resolver_set_ns(resolver, 1, &nameserver,
					&nameserver_port);
	if (status != PJ_SUCCESS) {
		fail_status("pj_dns_resolver_set_ns", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pj_dns_resolver_get_settings(resolver, &dns_settings);
	if (status == PJ_SUCCESS) {
		dns_settings.qretr_delay = 50;
		dns_settings.qretr_count = 3;
		dns_settings.cache_max_ttl = 60;
		dns_settings.good_ns_ttl = 1;
		dns_settings.bad_ns_ttl = 0;
		status = pj_dns_resolver_set_settings(resolver, &dns_settings);
	}
	if (status == PJ_SUCCESS)
		status = pjsip_endpt_set_resolver(endpoint, resolver);
	if (status != PJ_SUCCESS) {
		fail_status("attach DNS resolver", __LINE__, status);
		goto destroy_endpoint;
	}
	if (pjsip_endpt_get_resolver(endpoint) != resolver) {
		fail_value("attach DNS resolver", __LINE__,
			   "endpoint returns attached resolver");
		goto destroy_endpoint;
	}

	thread_pool = pjsip_endpt_create_pool(endpoint, "phase9-thread", 8192,
					      4096);
	test_pool = pjsip_endpt_create_pool(endpoint, "phase9-test", 16384,
					    4096);
	if (thread_pool == NULL || test_pool == NULL) {
		fail_value("Phase 9 pools", __LINE__,
			   "thread and test pools exist");
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p9-dns", phase9_dns_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.dns_thread);
	if (status == PJ_SUCCESS)
		status = pj_thread_create(thread_pool, "p9-event",
					  phase9_event_thread, &context,
					  PJ_THREAD_DEFAULT_STACK_SIZE, 0,
					  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("Phase 9 threads", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.dns_started, 1,
			   PHASE9_WAIT_MS) != 0 ||
	    wait_for_count(&context, &context.event_started, 1,
			   PHASE9_WAIT_MS) != 0) {
		fail_value("Phase 9 threads", __LINE__, "both threads started");
		goto destroy_endpoint;
	}
	printk("[Phase 9] local DNS responder 127.0.0.1:%u attached to endpoint resolver: PASSED\n",
	       context.dns_port);

	if (test_a_lookup(&context, test_pool) != 0 ||
	    test_srv_lookup(&context, test_pool) != 0 ||
	    test_resolved_transport(&context) != 0 ||
	    test_numeric_fallback(&context, test_pool) != 0 ||
	    test_missing(&context, test_pool) != 0 ||
	    test_timeout(&context, test_pool) != 0 ||
	    test_cancel(&context) != 0 ||
	    test_malformed(&context, test_pool) != 0)
		goto destroy_endpoint;
	if (atomic_get(&context.dns_aaaa_queries) != 0 ||
	    atomic_get(&context.dns_naptr_queries) != 0 ||
	    atomic_get(&context.dns_unexpected_queries) != 0) {
		fail_value("IPv4 resolver scope", __LINE__,
			   "no AAAA, NAPTR, or unexpected DNS queries");
		goto destroy_endpoint;
	}
	printk("[Phase 9] IPv6/AAAA disabled and NAPTR explicitly deferred: PASSED\n");
	result = 0;

destroy_endpoint:
	atomic_set(&context.teardown_started, 1);
	if (resolver_created) {
		status = pjsip_endpt_set_resolver(endpoint, NULL);
		if (status == PJ_SUCCESS)
			status = pj_dns_resolver_destroy(resolver, PJ_TRUE);
		if (status != PJ_SUCCESS) {
			fail_status("resolver destruction", __LINE__, status);
			result = -1;
		}
		resolver_created = PJ_FALSE;
		resolver = NULL;
		context.resolver = NULL;
		if (context.event_thread != NULL)
			pj_thread_sleep(PJ_IOQUEUE_KEY_FREE_DELAY + 50);
		if (result == 0 &&
		    (pjsip_endpt_get_resolver(endpoint) != NULL ||
		     atomic_get(&context.event_error) != 0 ||
		     atomic_get(&context.callback_error) != 0)) {
			fail_value("resolver destruction", __LINE__,
				   "detached without stale callbacks or event errors");
			result = -1;
		}
		if (result == 0)
			printk("[Phase 9] resolver cancellation cache/socket/timer destruction while polling: PASSED\n");
	}
	if (context.dns_thread != NULL) {
		atomic_set(&context.dns_stop, 1);
		status = pj_thread_join(context.dns_thread);
		if (status == PJ_SUCCESS)
			status = pj_thread_destroy(context.dns_thread);
		if (status != PJ_SUCCESS) {
			fail_status("DNS responder thread teardown", __LINE__, status);
			result = -1;
		}
		context.dns_thread = NULL;
	}
	if (context.dns_socket != PJ_INVALID_SOCKET) {
		status = pj_sock_close(context.dns_socket);
		if (status != PJ_SUCCESS) {
			fail_status("DNS responder socket close", __LINE__, status);
			result = -1;
		}
		context.dns_socket = PJ_INVALID_SOCKET;
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
			   PHASE9_WAIT_MS) != 0) {
		fail_value("UDP transport teardown", __LINE__,
			   "two transports destroyed while polling");
		result = -1;
	}
	context.client_udp = NULL;
	context.server_udp = NULL;
	client_udp = NULL;
	server_udp = NULL;
	if (context.event_thread != NULL &&
	    (wait_for_transactions(&context, 0, PHASE9_WAIT_MS) != 0 ||
	     wait_for_timers(&context, 0, PHASE9_WAIT_MS) != 0)) {
		fail_value("Phase 9 teardown", __LINE__,
			   "transactions and timers drained");
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
		pjsip_tpmgr_set_state_cb(transport_manager,
					 context.previous_state_cb);
		callbacks_installed = PJ_FALSE;
	}
	if (module_registered &&
	    (!tsx_layer_initialized || pjsip_tsx_layer_get_tsx_count() == 0)) {
		status = pjsip_endpt_unregister_module(endpoint, &phase9_module);
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
	if (test_pool != NULL) {
		pjsip_endpt_release_pool(endpoint, test_pool);
		test_pool = NULL;
	}
	if (thread_pool != NULL) {
		pjsip_endpt_release_pool(endpoint, thread_pool);
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
		printk("[Phase 9] lifecycle %d resolver/transports/threads/endpoint teardown: PASSED (select close-race retries=%d)\n",
		       iteration,
		       (int)atomic_get(&context.teardown_poll_retries));

destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 9] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase9_resolver_run(void)
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
	printk("[Phase 9] deterministic IPv4 SIP resolution validation (%d lifecycles)\n",
	       PHASE9_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE9_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			result = -1;
			break;
		}
	}
	pjsip_cfg()->tsx.t1 = saved_t1;
	pjsip_cfg()->tsx.t2 = saved_t2;
	pjsip_cfg()->tsx.t4 = saved_t4;
	pjsip_cfg()->tsx.td = saved_td;
	if (result == 0)
		printk("PHASE 9 RESULT: PASSED (%d/%d lifecycles)\n",
		       PHASE9_LIFECYCLES, PHASE9_LIFECYCLES);
	else
		printk("PHASE 9 RESULT: FAILED at lifecycle %d\n", iteration);
	return result == 0 ? 0 : 1;
}
