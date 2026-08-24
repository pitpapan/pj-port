#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <pjlib.h>
#include <pjlib-util.h>
#include <pjmedia/endpoint.h>
#include <pjmedia/rtp.h>
#include <pjmedia/transport.h>
#include <pjmedia/transport_loop.h>
#include <pjmedia/transport_udp.h>
#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pjsip-ua/sip_regc.h>

#include <errno.h>

#define PHASE9_LIFECYCLES 3
#define PHASE9_WAIT_MS 2500
#define PHASE9_MAX_EXTRA_TRANSPORTS 7
#define PHASE9_MAX_RAW_SOCKETS 32

_Static_assert(PJ_HAS_IPV6 == 0, "Phase 9 is IPv4-only");
_Static_assert(PJSIP_MAX_TRANSPORTS == 16,
	       "Phase 9 capacity report expects 16 endpoint ioqueue handles");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "Phase 9 excludes SRTP");
_Static_assert(PJMEDIA_HAS_RTCP_XR == 0, "Phase 9 excludes RTCP XR");

static const pj_uint8_t rtp_packet[] = {
	0x80, 0x00, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
	0x11, 0x22, 0x33, 0x44, 0x10, 0x20, 0x30, 0x40,
};
static const pj_uint8_t rtcp_packet[] = {
	0x80, 0xc9, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44,
};

struct phase9_context;

struct phase9_user {
	struct phase9_context *context;
	unsigned id;
	atomic_t rtp_count;
	atomic_t rtcp_count;
	atomic_t rejected_count;
	atomic_t wrong_user;
	atomic_t wrong_packet;
	atomic_t source_seen;
};

struct phase9_context {
	pjsip_endpoint *sip_endpt;
	pjsip_transport *sip_server;
	pjsip_transport *sip_client;
	pjmedia_endpt *media_endpt;
	pj_thread_t *event_thread;
	pjsip_regc *registration;
	struct phase9_user users[3];
	atomic_t event_started;
	atomic_t event_stop;
	atomic_t event_error;
	atomic_t close_race_retries;
	atomic_t callback_error;
	atomic_t registration_requests;
	atomic_t registration_callbacks;
	atomic_t registration_code;
	atomic_t late_callbacks;
	atomic_t event_stack_status;
	atomic_t event_stack_unused;
};

static struct phase9_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 9] FAIL %s:%d status=%d (%s)\n", test, line, status,
	       text);
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

static void make_deadline(pj_time_val *deadline, unsigned timeout_ms)
{
	pj_gettimeofday(deadline);
	deadline->msec += timeout_ms;
	pj_time_val_normalize(deadline);
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
			return -1;
		{
			pj_time_val now;

			pj_gettimeofday(&now);
			if (PJ_TIME_VAL_GTE(now, deadline))
				return -1;
		}
		k_msleep(1);
	}
	return 0;
}

static void record_callback_error(int error)
{
	if (active_context != NULL)
		atomic_cas(&active_context->callback_error, 0, error);
}

static void rtp_callback2(pjmedia_tp_cb_param *param)
{
	struct phase9_user *user = param != NULL ? param->user_data : NULL;

	if (user == NULL || user->context != active_context) {
		record_callback_error(-901);
		return;
	}
	if (param->size == (pj_ssize_t)sizeof(rtp_packet) &&
	    pj_memcmp(param->pkt, rtp_packet, sizeof(rtp_packet)) == 0) {
		atomic_inc(&user->rtp_count);
	} else if (param->size == (pj_ssize_t)sizeof(rtcp_packet) &&
		   pj_memcmp(param->pkt, rtcp_packet, sizeof(rtcp_packet)) == 0) {
		/* RTCP-mux arrives on the RTP socket and is demultiplexed by the
		 * future stream. At this phase, prove the socket route explicitly. */
		atomic_inc(&user->rtcp_count);
	} else {
		atomic_inc(&user->rejected_count);
	}
	if (param->src_addr != NULL && pj_sockaddr_has_addr(param->src_addr))
		atomic_set(&user->source_seen, 1);
}

static void rtp_callback(void *user_data, void *packet, pj_ssize_t size)
{
	pjmedia_tp_cb_param param;

	pj_bzero(&param, sizeof(param));
	param.user_data = user_data;
	param.pkt = packet;
	param.size = size;
	rtp_callback2(&param);
}

static void rtcp_callback(void *user_data, void *packet, pj_ssize_t size)
{
	struct phase9_user *user = user_data;

	if (user == NULL || user->context != active_context) {
		record_callback_error(-902);
		return;
	}
	if (size == (pj_ssize_t)sizeof(rtcp_packet) &&
	    pj_memcmp(packet, rtcp_packet, sizeof(rtcp_packet)) == 0)
		atomic_inc(&user->rtcp_count);
	else
		atomic_inc(&user->rejected_count);
}

static int event_thread_main(void *arg)
{
	struct phase9_context *context = arg;
	size_t unused = 0;
	pj_status_t status = PJ_SUCCESS;

	atomic_set(&context->event_started, 1);
	while (!atomic_get(&context->event_stop)) {
		pj_time_val timeout = {0, 10};

		status = pjsip_endpt_handle_events(context->sip_endpt, &timeout);
		if (status != PJ_SUCCESS) {
			if (status == PJ_STATUS_FROM_OS(EBADF)) {
				atomic_inc(&context->close_race_retries);
				pj_thread_sleep(1);
				continue;
			}
			atomic_set(&context->event_error, status);
			break;
		}
	}
	atomic_set(&context->event_stack_status,
		   k_thread_stack_space_get(k_current_get(), &unused));
	atomic_set(&context->event_stack_unused, (atomic_val_t)unused);
	return 0;
}

static pj_bool_t on_rx_request(pjsip_rx_data *rdata)
{
	pj_status_t status;

	if (active_context == NULL || rdata == NULL ||
	    rdata->tp_info.transport != active_context->sip_server ||
	    rdata->msg_info.msg->line.req.method.id != PJSIP_REGISTER_METHOD)
		return PJ_FALSE;
	atomic_inc(&active_context->registration_requests);
	status = pjsip_endpt_respond_stateless(active_context->sip_endpt, rdata,
					      200, NULL, NULL, NULL);
	if (status != PJ_SUCCESS)
		record_callback_error(status);
	return PJ_TRUE;
}

static pjsip_module phase9_module = {
	.name = {"phase9-media", 12},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_rx_request = on_rx_request,
};

static void registration_callback(struct pjsip_regc_cbparam *param)
{
	struct phase9_context *context = param != NULL ? param->token : NULL;

	if (context == NULL) {
		record_callback_error(-903);
		return;
	}
	atomic_set(&context->registration_code, param->code);
	atomic_inc(&context->registration_callbacks);
}

static pj_status_t send_registration(struct phase9_context *context,
				     pj_bool_t unregister)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = unregister ?
		pjsip_regc_unregister(context->registration, &tdata) :
		pjsip_regc_register(context->registration, PJ_TRUE, &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_send(context->registration, tdata);
	return status;
}

static pj_status_t start_registration(struct phase9_context *context)
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
			     context->sip_server->local_name.port) <= 0 ||
	    pj_ansi_snprintf(identity_text, sizeof(identity_text),
			     "<sip:phase9@127.0.0.1:%u>",
			     context->sip_client->local_name.port) <= 0 ||
	    pj_ansi_snprintf(contact_text, sizeof(contact_text),
			     "<sip:phase9@127.0.0.1:%u;transport=udp>",
			     context->sip_client->local_name.port) <= 0)
		return PJ_ETOOSMALL;
	registrar = pj_str(registrar_text);
	identity = pj_str(identity_text);
	contact = pj_str(contact_text);
	status = pjsip_regc_create(context->sip_endpt, context,
				   registration_callback, &context->registration);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_init(context->registration, &registrar,
				  &identity, &identity, 1, &contact, 30);
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->sip_client;
	if (status == PJ_SUCCESS)
		status = pjsip_regc_set_transport(context->registration, &selector);
	if (status == PJ_SUCCESS)
		status = send_registration(context, PJ_FALSE);
	return status;
}

static pj_status_t make_bound_socket(pj_sock_t *socket, pj_sockaddr *address)
{
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	int address_length = sizeof(*address);
	pj_status_t status;

	*socket = PJ_INVALID_SOCKET;
	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_DGRAM() | pj_SOCK_CLOEXEC(),
				0, socket);
	if (status == PJ_SUCCESS)
		status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pj_sock_bind(*socket, &bind_address,
				      sizeof(bind_address));
	if (status == PJ_SUCCESS)
		status = pj_sock_getsockname(*socket, address, &address_length);
	if (status != PJ_SUCCESS && *socket != PJ_INVALID_SOCKET) {
		pj_sock_close(*socket);
		*socket = PJ_INVALID_SOCKET;
	}
	return status;
}

static pj_status_t create_udp_transport(pjmedia_endpt *endpt,
					const char *name,
					pjmedia_transport **transport)
{
	pjmedia_sock_info socket_info;
	pj_status_t status;
	pj_bool_t attach_attempted = PJ_FALSE;

	pj_bzero(&socket_info, sizeof(socket_info));
	socket_info.rtp_sock = socket_info.rtcp_sock = PJ_INVALID_SOCKET;
	status = make_bound_socket(&socket_info.rtp_sock,
				   &socket_info.rtp_addr_name);
	if (status == PJ_SUCCESS)
		status = make_bound_socket(&socket_info.rtcp_sock,
					   &socket_info.rtcp_addr_name);
	if (status == PJ_SUCCESS) {
		attach_attempted = PJ_TRUE;
		status = pjmedia_transport_udp_attach(endpt, name, &socket_info,
						      PJMEDIA_UDP_NO_SRC_ADDR_CHECKING,
						      transport);
	}
	if (attach_attempted) {
		/* The UDP transport owns both sockets once attach is attempted,
		 * including its internal error path. */
		socket_info.rtp_sock = socket_info.rtcp_sock = PJ_INVALID_SOCKET;
	}
	if (status != PJ_SUCCESS) {
		if (socket_info.rtp_sock != PJ_INVALID_SOCKET)
			pj_sock_close(socket_info.rtp_sock);
		if (socket_info.rtcp_sock != PJ_INVALID_SOCKET)
			pj_sock_close(socket_info.rtcp_sock);
	}
	return status;
}

static void init_attach_param(pjmedia_transport_attach_param *param,
			      struct phase9_user *user,
			      const pjmedia_transport_info *remote,
			      pj_bool_t mux)
{
	pj_bzero(param, sizeof(*param));
	param->media_type = PJMEDIA_TYPE_AUDIO;
	param->user_data = user;
	pj_sockaddr_cp(&param->rem_addr, &remote->sock_info.rtp_addr_name);
	pj_sockaddr_cp(&param->rem_rtcp, mux ?
		       &remote->sock_info.rtp_addr_name :
		       &remote->sock_info.rtcp_addr_name);
	param->addr_len = sizeof(pj_sockaddr_in);
	param->rtp_cb2 = rtp_callback2;
	param->rtcp_cb = rtcp_callback;
}

static int test_loop_transport(struct phase9_context *context)
{
	const char *test = "loop transport";
	pjmedia_loop_tp_setting setting;
	pjmedia_transport *transport = NULL;
	pjmedia_transport_info info;
	pjmedia_transport_attach_param param;
	pj_sockaddr remote;
	pj_str_t loopback = pj_str("127.0.0.1");
	struct phase9_user *first = &context->users[0];
	struct phase9_user *second = &context->users[1];
	atomic_val_t first_rtp;
	atomic_val_t first_rtcp;

	pjmedia_loop_tp_setting_default(&setting);
	setting.max_attach_cnt = 2;
	setting.port = 42000;
	CHECK_STATUS(test, pjmedia_transport_loop_create2(context->media_endpt,
							&setting, &transport));
	pjmedia_transport_info_init(&info);
	CHECK_STATUS(test, pjmedia_transport_get_info(transport, &info));
	CHECK_TRUE(test, pj_sockaddr_get_port(&info.sock_info.rtp_addr_name) ==
			 42000);
	CHECK_STATUS(test, pj_sockaddr_init(pj_AF_INET(), &remote, &loopback,
					   42000));
	pj_bzero(&param, sizeof(param));
	param.user_data = first;
	pj_sockaddr_cp(&param.rem_addr, &remote);
	pj_sockaddr_cp(&param.rem_rtcp, &remote);
	param.addr_len = sizeof(pj_sockaddr_in);
	param.rtp_cb2 = rtp_callback2;
	param.rtcp_cb = rtcp_callback;
	CHECK_STATUS(test, pjmedia_transport_attach2(transport, &param));
	CHECK_STATUS(test, pjmedia_transport_attach(
		transport, second, &remote, &remote, sizeof(pj_sockaddr_in),
		rtp_callback, rtcp_callback));
	CHECK_STATUS(test, pjmedia_transport_send_rtp(transport, rtp_packet,
						      sizeof(rtp_packet)));
	CHECK_STATUS(test, pjmedia_transport_send_rtcp(transport, rtcp_packet,
						       sizeof(rtcp_packet)));
	CHECK_TRUE(test, atomic_get(&first->rtp_count) == 1 &&
			 atomic_get(&second->rtp_count) == 1 &&
			 atomic_get(&first->rtcp_count) == 1 &&
			 atomic_get(&second->rtcp_count) == 1);
	CHECK_STATUS(test, pjmedia_transport_loop_disable_rx(transport, second,
							     PJ_TRUE));
	CHECK_STATUS(test, pjmedia_transport_send_rtp(transport, rtp_packet,
						      sizeof(rtp_packet)));
	CHECK_TRUE(test, atomic_get(&first->rtp_count) == 2 &&
			 atomic_get(&second->rtp_count) == 1);
	pjmedia_transport_detach(transport, first);
	first_rtp = atomic_get(&first->rtp_count);
	first_rtcp = atomic_get(&first->rtcp_count);
	CHECK_STATUS(test, pjmedia_transport_send_rtp(transport, rtp_packet,
						      sizeof(rtp_packet)));
	CHECK_TRUE(test, atomic_get(&first->rtp_count) == first_rtp &&
			 atomic_get(&first->rtcp_count) == first_rtcp);
	pjmedia_transport_detach(transport, second);
	CHECK_STATUS(test, pjmedia_transport_close(transport));
	printk("[Phase 9] loop attach2/attach, ownership, fanout, disable, detach, and close: PASSED\n");
	return 0;
}

static int test_udp_transport(struct phase9_context *context)
{
	const char *test = "UDP transport";
	pjmedia_transport *first = NULL;
	pjmedia_transport *second = NULL;
	pjmedia_transport_info first_info;
	pjmedia_transport_info second_info;
	pjmedia_transport_attach_param param;
	pj_uint8_t oversized[PJMEDIA_MAX_MTU + 64];
	pj_uint8_t malformed[] = {0x00, 0x01, 0x02};
	pj_sock_t raw_socket = PJ_INVALID_SOCKET;
	pj_sockaddr raw_address;
	pj_ssize_t sent;
	atomic_val_t before;
	int result = -1;

	CHECK_STATUS(test, create_udp_transport(context->media_endpt, "p9-a",
						&first));
	CHECK_STATUS(test, create_udp_transport(context->media_endpt, "p9-b",
						&second));
	pjmedia_transport_info_init(&first_info);
	pjmedia_transport_info_init(&second_info);
	CHECK_STATUS(test, pjmedia_transport_get_info(first, &first_info));
	CHECK_STATUS(test, pjmedia_transport_get_info(second, &second_info));
	CHECK_TRUE(test, pj_sockaddr_get_port(&first_info.sock_info.rtp_addr_name) != 0 &&
			 pj_sockaddr_get_port(&first_info.sock_info.rtcp_addr_name) != 0 &&
			 pj_sockaddr_get_port(&second_info.sock_info.rtp_addr_name) != 0 &&
			 pj_sockaddr_get_port(&second_info.sock_info.rtcp_addr_name) != 0);
	init_attach_param(&param, &context->users[0], &second_info, PJ_FALSE);
	CHECK_STATUS(test, pjmedia_transport_attach2(first, &param));
	init_attach_param(&param, &context->users[1], &first_info, PJ_FALSE);
	CHECK_STATUS(test, pjmedia_transport_attach2(second, &param));
	CHECK_STATUS(test, pjmedia_transport_media_start(first, NULL, NULL, NULL,
							 0));
	CHECK_STATUS(test, pjmedia_transport_media_start(second, NULL, NULL, NULL,
							  0));
	before = atomic_get(&context->users[1].rtp_count);
	CHECK_STATUS(test, pjmedia_transport_send_rtp(first, rtp_packet,
						      sizeof(rtp_packet)));
	CHECK_TRUE(test, wait_for_value(&context->users[1].rtp_count, before + 1,
					 PHASE9_WAIT_MS) == 0);
	before = atomic_get(&context->users[0].rtcp_count);
	CHECK_STATUS(test, pjmedia_transport_send_rtcp(second, rtcp_packet,
						       sizeof(rtcp_packet)));
	CHECK_TRUE(test, wait_for_value(&context->users[0].rtcp_count, before + 1,
					 PHASE9_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&context->users[1].source_seen) == 1);
	pjmedia_transport_info_init(&first_info);
	CHECK_STATUS(test, pjmedia_transport_get_info(first, &first_info));
	CHECK_TRUE(test, pj_sockaddr_get_port(&first_info.src_rtcp_name) ==
			 pj_sockaddr_get_port(&second_info.sock_info.rtcp_addr_name));
	printk("[Phase 9] distinct ephemeral IPv4 RTP/RTCP sockets and source address: PASSED\n");

	/* Inject malformed and oversized datagrams without violating the public
	 * send API's debug precondition (size <= PJMEDIA_MAX_MTU). */
	pj_bzero(oversized, sizeof(oversized));
	CHECK_STATUS(test, make_bound_socket(&raw_socket, &raw_address));
	before = atomic_get(&context->users[0].rejected_count);
	sent = sizeof(malformed);
	CHECK_STATUS(test, pj_sock_sendto(raw_socket, malformed, &sent, 0,
					  &first_info.sock_info.rtp_addr_name,
					  sizeof(pj_sockaddr_in)));
	CHECK_TRUE(test, wait_for_value(&context->users[0].rejected_count,
					 before + 1, PHASE9_WAIT_MS) == 0);
	before = atomic_get(&context->users[0].rejected_count);
	sent = sizeof(oversized);
	{
		pj_status_t oversized_status = pj_sock_sendto(
			raw_socket, oversized, &sent, 0,
			&first_info.sock_info.rtp_addr_name, sizeof(pj_sockaddr_in));

		CHECK_TRUE(test, oversized_status != PJ_SUCCESS);
	}
	CHECK_TRUE(test, atomic_get(&context->users[0].rejected_count) == before);
	CHECK_STATUS(test, pj_sock_close(raw_socket));
	raw_socket = PJ_INVALID_SOCKET;
	printk("[Phase 9] malformed callback rejection and oversized socket-boundary rejection: PASSED\n");

	/* Exercise mux separately: RTCP goes to the peer RTP socket. */
	CHECK_STATUS(test, pjmedia_transport_media_stop(first));
	CHECK_STATUS(test, pjmedia_transport_media_stop(second));
	pjmedia_transport_detach(first, &context->users[0]);
	pjmedia_transport_detach(second, &context->users[1]);
	init_attach_param(&param, &context->users[0], &second_info, PJ_TRUE);
	CHECK_STATUS(test, pjmedia_transport_attach2(first, &param));
	init_attach_param(&param, &context->users[1], &first_info, PJ_TRUE);
	CHECK_STATUS(test, pjmedia_transport_attach2(second, &param));
	CHECK_STATUS(test, pjmedia_transport_media_start(first, NULL, NULL, NULL, 0));
	CHECK_STATUS(test, pjmedia_transport_media_start(second, NULL, NULL, NULL, 0));
	before = atomic_get(&context->users[1].rtcp_count);
	CHECK_STATUS(test, pjmedia_transport_send_rtcp(first, rtcp_packet,
						       sizeof(rtcp_packet)));
	CHECK_TRUE(test, wait_for_value(&context->users[1].rtcp_count, before + 1,
					 PHASE9_WAIT_MS) == 0);
	printk("[Phase 9] RTCP-mux routed independently over the RTP socket: PASSED\n");

	CHECK_STATUS(test, pjmedia_transport_media_stop(first));
	CHECK_STATUS(test, pjmedia_transport_media_stop(second));
	pjmedia_transport_detach(first, &context->users[0]);
	pjmedia_transport_detach(second, &context->users[1]);
	before = atomic_get(&context->users[1].rtp_count) +
		 atomic_get(&context->users[1].rtcp_count);
	CHECK_STATUS(test, pjmedia_transport_close(first));
	first = NULL;
	CHECK_STATUS(test, pjmedia_transport_close(second));
	second = NULL;
	k_msleep(50);
	CHECK_TRUE(test, atomic_get(&context->users[1].rtp_count) +
			 atomic_get(&context->users[1].rtcp_count) == before);
	printk("[Phase 9] receive cancellation and destruction while shared polling quiesced: PASSED\n");
	result = 0;

	if (raw_socket != PJ_INVALID_SOCKET)
		pj_sock_close(raw_socket);
	if (first != NULL)
		pjmedia_transport_close(first);
	if (second != NULL)
		pjmedia_transport_close(second);
	return result;
}

static int test_capacity(struct phase9_context *context)
{
	const char *test = "capacity";
	pjmedia_transport *transports[PHASE9_MAX_EXTRA_TRANSPORTS] = {0};
	pjmedia_transport *recovery = NULL;
	unsigned count;

	/* Safe-unregistration defers key reclamation briefly. Let the two prior
	 * RTP/RTCP pairs leave the ioqueue before measuring its live ceiling. */
	k_msleep(600);
	for (count = 0; count < PJ_ARRAY_SIZE(transports); ++count) {
		CHECK_STATUS(test, create_udp_transport(context->media_endpt,
						"p9-cap", &transports[count]));
		printk("[Phase 9] capacity transport %u created (%u media sockets)\n",
		       count + 1, (count + 1) * 2);
	}
	for (unsigned i = 0; i < count; ++i)
		CHECK_STATUS(test, pjmedia_transport_close(transports[i]));
	k_msleep(600);
	CHECK_STATUS(test, create_udp_transport(context->media_endpt, "p9-recover",
						&recovery));
	CHECK_STATUS(test, pjmedia_transport_close(recovery));
	printk("[Phase 9] capacity: 7 media transports/14 handles with 2 SIP handles; release/recovery: PASSED\n");
	return 0;
}

static int test_socket_capacity(void)
{
	const char *test = "socket capacity";
	pj_sock_t sockets[PHASE9_MAX_RAW_SOCKETS];
	pj_sock_t recovery_socket = PJ_INVALID_SOCKET;
	pj_sockaddr address;
	pj_status_t failure = PJ_SUCCESS;
	unsigned count;

	for (count = 0; count < PJ_ARRAY_SIZE(sockets); ++count)
		sockets[count] = PJ_INVALID_SOCKET;
	for (count = 0; count < PJ_ARRAY_SIZE(sockets); ++count) {
		failure = make_bound_socket(&sockets[count], &address);
		if (failure != PJ_SUCCESS)
			break;
	}
	CHECK_TRUE(test, count > 0 && count < PJ_ARRAY_SIZE(sockets));
	for (unsigned i = 0; i < count; ++i)
		CHECK_STATUS(test, pj_sock_close(sockets[i]));
	CHECK_STATUS(test, make_bound_socket(&recovery_socket, &address));
	CHECK_STATUS(test, pj_sock_close(recovery_socket));
	printk("[Phase 9] descriptor/context capacity: %u extra bound sockets before status=%d; release/recovery: PASSED\n",
	       count, failure);
	return 0;
}

static int run_lifecycle(unsigned lifecycle)
{
	struct phase9_context context;
	pj_caching_pool caching_pool;
	pjmedia_endpt *media_endpt = NULL;
	pj_pool_t *thread_pool = NULL;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_status_t status;
	pj_bool_t pool_initialized = PJ_FALSE;
	pj_bool_t module_registered = PJ_FALSE;
	int result = -1;

	pj_bzero(&context, sizeof(context));
	for (unsigned i = 0; i < PJ_ARRAY_SIZE(context.users); ++i) {
		context.users[i].context = &context;
		context.users[i].id = i + 1;
	}
	status = pj_init();
	if (status != PJ_SUCCESS)
		return fail_status("pj_init", __LINE__, status);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS)
		goto shutdown;
	pj_caching_pool_init(&caching_pool, NULL, 0);
	pool_initialized = PJ_TRUE;
	active_context = &context;
	status = pjsip_endpt_create(&caching_pool.factory, "phase9",
				    &context.sip_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_factory;
	status = pjsip_tsx_layer_init_module(context.sip_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	status = pjsip_endpt_register_module(context.sip_endpt, &phase9_module);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	module_registered = PJ_TRUE;
	status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pjsip_udp_transport_start(context.sip_endpt, &bind_address,
						   NULL, 1, &context.sip_server);
	if (status == PJ_SUCCESS)
		status = pjsip_udp_transport_start(context.sip_endpt, &bind_address,
						   NULL, 1, &context.sip_client);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	status = pjmedia_endpt_create2(&caching_pool.factory,
		pjsip_endpt_get_ioqueue(context.sip_endpt), 0, &media_endpt);
	if (status != PJ_SUCCESS)
		goto destroy_endpoint;
	context.media_endpt = media_endpt;
	if (pjmedia_endpt_get_ioqueue(media_endpt) !=
	    pjsip_endpt_get_ioqueue(context.sip_endpt) ||
	    pjmedia_endpt_get_thread_count(media_endpt) != 0) {
		fail_value("shared ioqueue", __LINE__, "same ioqueue and zero media workers");
		goto destroy_media;
	}
	thread_pool = pjsip_endpt_create_pool(context.sip_endpt, "p9-thread",
					      4096, 4096);
	if (thread_pool == NULL) {
		status = PJ_ENOMEM;
		goto destroy_media;
	}
	status = pj_thread_create(thread_pool, "p9-event", event_thread_main,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS)
		goto destroy_media;
	if (wait_for_value(&context.event_started, 1, PHASE9_WAIT_MS) != 0)
		goto destroy_media;
	status = start_registration(&context);
	if (status != PJ_SUCCESS)
		goto destroy_media;
	if (wait_for_value(&context.registration_callbacks, 1,
			   PHASE9_WAIT_MS) != 0 ||
	    atomic_get(&context.registration_code) != 200 ||
	    atomic_get(&context.registration_requests) != 1)
		goto destroy_media;
	printk("[Phase 9] active SIP registration and shared PJSIP/media ioqueue: PASSED\n");
	if (test_loop_transport(&context) != 0 ||
	    test_udp_transport(&context) != 0)
		goto destroy_media;
	status = send_registration(&context, PJ_TRUE);
	if (status != PJ_SUCCESS ||
	    wait_for_value(&context.registration_callbacks, 2,
			   PHASE9_WAIT_MS) != 0 ||
	    atomic_get(&context.registration_code) != 200 ||
	    atomic_get(&context.registration_requests) != 2)
		goto destroy_media;
	if (test_capacity(&context) != 0 || test_socket_capacity() != 0)
		goto destroy_media;
	result = 0;

destroy_media:
	if (context.registration != NULL) {
		status = pjsip_regc_destroy2(context.registration, PJ_TRUE);
		if (status != PJ_SUCCESS)
			result = -1;
		context.registration = NULL;
	}
	if (media_endpt != NULL) {
		status = pjmedia_endpt_destroy2(media_endpt);
		if (status != PJ_SUCCESS)
			result = -1;
		context.media_endpt = NULL;
	}
	if (context.sip_client != NULL) {
		pjsip_transport_shutdown(context.sip_client);
		context.sip_client = NULL;
	}
	if (context.sip_server != NULL) {
		pjsip_transport_shutdown(context.sip_server);
		context.sip_server = NULL;
	}
	if (context.event_thread != NULL) {
		atomic_set(&context.event_stop, 1);
		status = pj_thread_join(context.event_thread);
		if (status == PJ_SUCCESS)
			status = pj_thread_destroy(context.event_thread);
		if (status != PJ_SUCCESS)
			result = -1;
		if (atomic_get(&context.event_stack_status) != 0)
			result = fail_value("event stack", __LINE__,
					    "stack watermark available");
		context.event_thread = NULL;
	}
	if (module_registered) {
		status = pjsip_endpt_unregister_module(context.sip_endpt,
						     &phase9_module);
		if (status != PJ_SUCCESS)
			result = -1;
	}
	if (thread_pool != NULL)
		pj_pool_release(thread_pool);
destroy_endpoint:
	if (context.sip_endpt != NULL)
		pjsip_endpt_destroy(context.sip_endpt);
	if (result == 0 && (caching_pool.used_count != 0 ||
			    caching_pool.capacity != 0))
		result = fail_value("pool teardown", __LINE__, "caching pool empty");
destroy_factory:
	active_context = NULL;
	if (pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	if (status != PJ_SUCCESS && result != 0)
		return fail_status("lifecycle", __LINE__, status);
	if (result == 0) {
		printk("[Phase 9] lifecycle %u event stack: configured=%u B, used<=%u B, unused=%u B; close-race retries=%d\n",
		       lifecycle, (unsigned)CONFIG_DYNAMIC_THREAD_STACK_SIZE,
		       (unsigned)(CONFIG_DYNAMIC_THREAD_STACK_SIZE -
				  atomic_get(&context.event_stack_unused)),
		       (unsigned)atomic_get(&context.event_stack_unused),
		       (int)atomic_get(&context.close_race_retries));
		printk("[Phase 9] lifecycle %u transport/socket/callback teardown: PASSED\n",
		       lifecycle);
	}
	return result;
}

int phase9_transport_run(void)
{
	size_t stack_unused = 0;
	unsigned lifecycle;

	printk("[Phase 9] PJMEDIA loop and IPv4 UDP transport validation (%u lifecycles)\n",
	       PHASE9_LIFECYCLES);
	for (lifecycle = 1; lifecycle <= PHASE9_LIFECYCLES; ++lifecycle) {
		if (run_lifecycle(lifecycle) != 0) {
			printk("PHASE 9 RESULT: FAILED at lifecycle %u\n", lifecycle);
			return 1;
		}
	}
	if (k_thread_stack_space_get(k_current_get(), &stack_unused) != 0)
		return fail_value("main stack", __LINE__, "stack watermark available");
	printk("[Phase 9] main stack: configured=%u B, used<=%u B, unused=%u B\n",
	       (unsigned)CONFIG_MAIN_STACK_SIZE,
	       (unsigned)(CONFIG_MAIN_STACK_SIZE - stack_unused),
	       (unsigned)stack_unused);
	printk("PHASE 9 RESULT: PASSED (3 loop/UDP media transport lifecycles)\n");
	return 0;
}
