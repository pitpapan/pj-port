#include <pjsua-lib/pjsua.h>
#include <pj/sock_select.h>
#include <pj_zephyr_pool_arena.h>
#include <pjsip/sip_transaction.h>
#include <pjsip/sip_ua_layer.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <string.h>

_Static_assert(PJSUA_MAX_ACC == 5, "PJSUA_MAX_ACC must be five");
_Static_assert(PJSUA_MAX_CALLS == 7, "PJSUA_MAX_CALLS must be seven");
_Static_assert(PJSUA_MAX_CONF_PORTS == 12,
	       "PJSUA_MAX_CONF_PORTS must be twelve");
_Static_assert(PJMEDIA_HAS_SRTP == 0, "PJMEDIA SRTP must remain disabled");
_Static_assert(PJSIP_HAS_TLS_TRANSPORT == 0,
	       "PJSIP TLS transport must remain disabled");

#define ACCOUNT_COUNT 5
#define HELD_CALL_COUNT 7
#define EVENT_POLL_LIMIT 500
#define RESPONSE_POLL_MS 1000
#define QUIESCENCE_POLL_LIMIT 500

static pjsua_acc_id account_ids[ACCOUNT_COUNT];
static pjsua_call_id held_call_ids[HELD_CALL_COUNT];
static int account_sentinels[ACCOUNT_COUNT];
static int call_sentinels[HELD_CALL_COUNT];
static atomic_t incoming_count;
static atomic_t callback_total;
static int failures;

#define CHECK(condition, message) do { \
	if (!(condition)) { \
		printk("PJSUA CAPACITY CHECK FAILED: %s\n", message); \
		++failures; \
	} \
} while (0)

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id,
				     pjsip_rx_data *rdata)
{
	PJ_UNUSED_ARG(acc_id);
	PJ_UNUSED_ARG(rdata);
	atomic_inc(&callback_total);
	if (atomic_get(&incoming_count) < HELD_CALL_COUNT) {
		held_call_ids[atomic_get(&incoming_count)] = call_id;
		atomic_inc(&incoming_count);
	}
}

static pj_status_t send_all(pj_sock_t sock, const char *data, pj_size_t len)
{
	pj_size_t sent_total = 0;

	while (sent_total < len) {
		pj_ssize_t sent = (pj_ssize_t)(len - sent_total);
		pj_status_t status = pj_sock_send(sock, data + sent_total, &sent, 0);
		if (status != PJ_SUCCESS || sent <= 0)
			return status == PJ_SUCCESS ? PJ_EUNKNOWN : status;
		sent_total += (pj_size_t)sent;
	}
	return PJ_SUCCESS;
}

static int make_invite(char *buffer, pj_size_t capacity, unsigned port,
			       unsigned sequence)
{
	return pj_ansi_snprintf(
		buffer, capacity,
		"INVITE sip:capacity@127.0.0.1:%u;transport=tcp SIP/2.0\r\n"
		"Via: SIP/2.0/TCP 127.0.0.1:9;branch=z9hG4bK-capacity-%u\r\n"
		"From: <sip:peer%u@127.0.0.1>;tag=capacity-%u\r\n"
		"To: <sip:capacity@127.0.0.1>\r\n"
		"Contact: <sip:peer%u@127.0.0.1:9;transport=tcp>\r\n"
		"Call-ID: pjsua-capacity-%u@127.0.0.1\r\n"
		"CSeq: %u INVITE\r\nMax-Forwards: 70\r\n"
		"Content-Length: 0\r\n\r\n",
		port, sequence, sequence, sequence, sequence, sequence, sequence);
}

static pj_status_t connect_peer(pj_sock_t *sock, unsigned port,
				unsigned sequence)
{
	pj_sockaddr_in address;
	pj_str_t loopback = pj_str((char *)"127.0.0.1");
	pj_status_t status;

	PJ_UNUSED_ARG(sequence);
	*sock = PJ_INVALID_SOCKET;
	status = pj_sock_socket(pj_AF_INET(), pj_SOCK_STREAM(), 0, sock);
	if (status != PJ_SUCCESS)
		return status;
	status = pj_sockaddr_in_init(&address, &loopback,
					(pj_uint16_t)port);
	if (status == PJ_SUCCESS)
		status = pj_sock_connect(*sock, &address, sizeof(address));
	if (status != PJ_SUCCESS) {
		pj_sock_close(*sock);
		*sock = PJ_INVALID_SOCKET;
	}
	return status;
}

static pj_status_t send_invite(pj_sock_t sock, unsigned port, unsigned sequence)
{
	char invite[768];
	int invite_len = make_invite(invite, sizeof(invite), port, sequence);

	if (invite_len <= 0)
		return PJ_EINVAL;
	return send_all(sock, invite, (pj_size_t)invite_len);
}

static int poll_incoming(unsigned expected)
{
	unsigned polls;

	for (polls = 0; polls < EVENT_POLL_LIMIT &&
		     (unsigned)atomic_get(&incoming_count) < expected; ++polls) {
		if (pjsua_handle_events(10) < 0)
			return -1;
	}
	return (unsigned)atomic_get(&incoming_count) == expected ? 0 : -1;
}

static void verify_held_calls(bool assign_sentinels)
{
	pjsua_call_id enumerated[HELD_CALL_COUNT];
	unsigned count = HELD_CALL_COUNT;
	pj_status_t status;
	unsigned i;

	status = pjsua_enum_calls(enumerated, &count);
	CHECK(status == PJ_SUCCESS && count == HELD_CALL_COUNT,
	      "enumerate seven calls");
	for (i = 0; i < HELD_CALL_COUNT; ++i) {
		pjsua_call_info info;

		if (assign_sentinels)
			call_sentinels[i] = (int)(0x7100 + i);
		if (assign_sentinels)
			CHECK(pjsua_call_set_user_data(held_call_ids[i],
					       &call_sentinels[i]) == PJ_SUCCESS,
			      "call user-data assignment");
		CHECK(pjsua_call_is_active(held_call_ids[i]) == PJ_TRUE,
		      "call remains independently addressable");
		CHECK(pjsua_call_get_user_data(held_call_ids[i]) ==
			      &call_sentinels[i], "call user-data sentinel");
		CHECK(pjsua_call_get_info(held_call_ids[i], &info) == PJ_SUCCESS &&
		      info.media_cnt == 0 && info.prov_media_cnt == 0,
		      "held call has no media");
		CHECK(enumerated[i] == held_call_ids[i],
		      "call ID remains enumerable");
	}
}

static int response_is_busy(pj_sock_t sock)
{
	pj_fd_set_t read_set;
	pj_time_val timeout = { 0, 10 };
	char response[2048];
	pj_size_t buffered = 0;
	int result;
	unsigned polls;

	for (polls = 0; polls < RESPONSE_POLL_MS / 10; ++polls) {
		if (pjsua_handle_events(10) < 0)
			return -1;
		PJ_FD_ZERO(&read_set);
		PJ_FD_SET(sock, &read_set);
		timeout.sec = 0;
		timeout.msec = 10;
		result = pj_sock_select((int)sock + 1, &read_set, NULL, NULL,
					&timeout);
		if (result <= 0 || !PJ_FD_ISSET(sock, &read_set))
			continue;
		if (buffered == sizeof(response) - 1)
			return -1;
		{
			pj_ssize_t length = sizeof(response) - 1 - buffered;

			if (pj_sock_recv(sock, response + buffered, &length, 0) !=
			    PJ_SUCCESS || length <= 0)
				return -1;
			buffered += (pj_size_t)length;
			response[buffered] = '\0';
		}
		for (;;) {
			char *end = strstr(response, "\r\n\r\n");
			pj_size_t consumed;

			if (end == NULL)
				break;
			*end = '\0';
			if (strncmp(response, "SIP/2.0 486 Busy Here\r\n",
				    sizeof("SIP/2.0 486 Busy Here\r\n") - 1) == 0 &&
			    strstr(response,
				   "Call-ID: pjsua-capacity-8@127.0.0.1\r\n") !=
				    NULL &&
			    strstr(response, "CSeq: 8 INVITE\r\n") != NULL)
				return 0;
			consumed = (pj_size_t)(end - response) + 4;
			memmove(response, response + consumed, buffered - consumed);
			buffered -= consumed;
			response[buffered] = '\0';
		}
	}
	return -1;
}

static int acknowledge_declines(pj_sock_t sock, unsigned port)
{
	char buffer[4096];
	pj_size_t buffered = 0;
	unsigned acknowledged = 0;
	unsigned polls;

	for (polls = 0; polls < EVENT_POLL_LIMIT &&
		     acknowledged < HELD_CALL_COUNT; ++polls) {
		pj_fd_set_t read_set;
		pj_time_val timeout = { 0, 10 };
		int result;

		(void)pjsua_handle_events(10);
		PJ_FD_ZERO(&read_set);
		PJ_FD_SET(sock, &read_set);
		result = pj_sock_select((int)sock + 1, &read_set, NULL, NULL,
					&timeout);
		if (result <= 0 || !PJ_FD_ISSET(sock, &read_set))
			continue;
		if (buffered == sizeof(buffer) - 1)
			return -1;
		{
			pj_ssize_t length = sizeof(buffer) - 1 - buffered;

			if (pj_sock_recv(sock, buffer + buffered, &length, 0) !=
			    PJ_SUCCESS || length <= 0)
				return -1;
			buffered += (pj_size_t)length;
			buffer[buffered] = '\0';
		}
		for (;;) {
			char *end = strstr(buffer, "\r\n\r\n");
			char to_header[256];
			char *to_start;
			char *to_end;
			char ack[768];
			int ack_len;

			if (end == NULL)
				break;
			*end = '\0';
			if (strstr(buffer, "SIP/2.0 603 Decline") == NULL) {
				pj_size_t consumed = (pj_size_t)(end - buffer) + 4;

				memmove(buffer, buffer + consumed,
					buffered - consumed);
				buffered -= consumed;
				buffer[buffered] = '\0';
				continue;
			}
			to_start = strstr(buffer, "To: ");
			to_end = to_start == NULL ? NULL : strstr(to_start, "\r\n");
			if (to_start == NULL || to_end == NULL ||
			    (pj_size_t)(to_end - to_start - 4) >=
				    sizeof(to_header))
				return -1;
			memcpy(to_header, to_start + 4,
			       (pj_size_t)(to_end - to_start - 4));
			to_header[to_end - to_start - 4] = '\0';
			ack_len = pj_ansi_snprintf(
				ack, sizeof(ack),
				"ACK sip:capacity@127.0.0.1:%u;transport=tcp SIP/2.0\r\n"
				"Via: SIP/2.0/TCP 127.0.0.1:9;branch=z9hG4bK-capacity-%u\r\n"
				"From: <sip:peer%u@127.0.0.1>;tag=capacity-%u\r\n"
				"To: %s\r\n"
				"Call-ID: pjsua-capacity-%u@127.0.0.1\r\n"
				"CSeq: %u ACK\r\nMax-Forwards: 70\r\n"
				"Content-Length: 0\r\n\r\n",
				port, acknowledged + 1, acknowledged + 1,
				acknowledged + 1, to_header, acknowledged + 1,
				acknowledged + 1);
			if (ack_len <= 0 ||
			    send_all(sock, ack, (pj_size_t)ack_len) != PJ_SUCCESS)
				return -1;
			++acknowledged;
			{
				pj_size_t consumed = (pj_size_t)(end - buffer) + 4;

				memmove(buffer, buffer + consumed,
					buffered - consumed);
				buffered -= consumed;
				buffer[buffered] = '\0';
			}
		}
	}
	return acknowledged == HELD_CALL_COUNT ? 0 : -1;
}

static int wait_for_cleanup_quiescence(unsigned transport_baseline)
{
	unsigned polls;

	for (polls = 0; polls < QUIESCENCE_POLL_LIMIT; ++polls) {
		if (pjsip_tsx_layer_get_tsx_count() == 0 &&
		    pjsip_ua_get_dlg_set_count() == 0 &&
		    pjsip_tpmgr_get_transport_count(
			    pjsip_endpt_get_tpmgr(pjsua_get_pjsip_endpt())) ==
			    transport_baseline)
			return 0;
		(void)pjsua_handle_events(10);
	}
	printk("PJSUA CAPACITY QUIESCENCE: transactions=%u dialogs=%u transports=%u expected=%u\n",
	       pjsip_tsx_layer_get_tsx_count(),
	       pjsip_ua_get_dlg_set_count(),
	       pjsip_tpmgr_get_transport_count(
		       pjsip_endpt_get_tpmgr(pjsua_get_pjsip_endpt())),
	       transport_baseline);
	return -1;
}

static int warmup_busy(pj_sock_t *sock, unsigned port)
{
	unsigned i;

	atomic_set(&incoming_count, 0);
	atomic_set(&callback_total, 0);
	if (connect_peer(sock, port, 0) != PJ_SUCCESS)
		return -1;
	for (i = 0; i < HELD_CALL_COUNT; ++i) {
		if (send_invite(*sock, port, i + 1) != PJ_SUCCESS ||
		    poll_incoming(i + 1) != 0)
			goto failed;
	}
	if (send_invite(*sock, port, HELD_CALL_COUNT + 1) != PJ_SUCCESS)
		goto failed;
	if (poll_incoming(HELD_CALL_COUNT) != 0 ||
	    response_is_busy(*sock) != 0)
		goto failed;
	for (i = 0; i < HELD_CALL_COUNT; ++i) {
		if (pjsua_call_hangup(held_call_ids[i], PJSIP_SC_DECLINE,
				      NULL, NULL) != PJ_SUCCESS)
			goto failed;
	}
	if (acknowledge_declines(*sock, port) != 0)
		goto failed;
	(void)pj_sock_close(*sock);
	*sock = PJ_INVALID_SOCKET;
	for (i = 0; i < EVENT_POLL_LIMIT; ++i)
		(void)pjsua_handle_events(10);
	return pjsua_call_get_count() == 0 ? 0 : -1;
failed:
	(void)pj_sock_close(*sock);
	*sock = PJ_INVALID_SOCKET;
	return -1;
}

static int run_capacity_test(void)
{
	pjsua_config ua_cfg;
	pjsua_logging_config log_cfg;
	pjsua_media_config media_cfg;
	pjsua_transport_config transport_cfg;
	pjsua_transport_info transport_info;
	pjsua_transport_id transport_id;
	pj_sock_t peers[HELD_CALL_COUNT + 1];
	struct pj_zephyr_pool_arena_stats baseline;
	struct pj_zephyr_pool_arena_stats cleanup;
	struct pj_zephyr_pool_arena_stats destroyed;
	bool baseline_valid = false;
	pj_status_t status;
	unsigned count;
	unsigned transport_baseline = 0;
	unsigned i;

	for (i = 0; i < HELD_CALL_COUNT + 1; ++i)
		peers[i] = PJ_INVALID_SOCKET;
	for (i = 0; i < ACCOUNT_COUNT; ++i)
		account_ids[i] = PJSUA_INVALID_ID;
	atomic_set(&incoming_count, 0);
	atomic_set(&callback_total, 0);

	status = pj_zephyr_pool_arena_install();
	CHECK(status == PJ_SUCCESS, "arena install");
	if (status != PJ_SUCCESS)
		return -1;
	status = pjsua_create();
	CHECK(status == PJ_SUCCESS, "pjsua create");
	if (status != PJ_SUCCESS)
		return -1;

	pjsua_config_default(&ua_cfg);
	pjsua_logging_config_default(&log_cfg);
	pjsua_media_config_default(&media_cfg);
	log_cfg.level = 6;
	ua_cfg.thread_cnt = 0;
	ua_cfg.max_calls = PJSUA_MAX_CALLS;
	ua_cfg.enable_unsolicited_mwi = PJ_FALSE;
	ua_cfg.stun_srv_cnt = 0;
	ua_cfg.enable_upnp = PJ_FALSE;
	ua_cfg.cb.on_incoming_call = &on_incoming_call;
	media_cfg.thread_cnt = 0;
	media_cfg.max_media_ports = PJSUA_MAX_CONF_PORTS;
	media_cfg.has_ioqueue = PJ_FALSE;
	media_cfg.conf_threads = 1;
	media_cfg.enable_ice = PJ_FALSE;
	media_cfg.enable_turn = PJ_FALSE;

	status = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
	CHECK(status == PJ_SUCCESS, "pjsua init");
	if (status != PJ_SUCCESS)
		goto destroy;
	CHECK(pjsua_set_no_snd_dev() != NULL, "disable sound device");

	pjsua_transport_config_default(&transport_cfg);
	transport_cfg.port = 0;
	transport_cfg.bound_addr = pj_str((char *)"127.0.0.1");
	status = pjsua_transport_create(PJSIP_TRANSPORT_TCP, &transport_cfg,
					&transport_id);
	CHECK(status == PJ_SUCCESS, "TCP transport create");
	if (status != PJ_SUCCESS)
		goto destroy;
	status = pjsua_transport_get_info(transport_id, &transport_info);
	CHECK(status == PJ_SUCCESS, "TCP transport info");
	if (status != PJ_SUCCESS)
		goto destroy;

	status = pjsua_start();
	CHECK(status == PJ_SUCCESS, "pjsua start");
	if (status != PJ_SUCCESS)
		goto cleanup;
	for (i = 0; i < 10; ++i)
		(void)pjsua_handle_events(10);
	transport_baseline = pjsip_tpmgr_get_transport_count(
		pjsip_endpt_get_tpmgr(pjsua_get_pjsip_endpt()));
	{
		pjsua_acc_config acc_cfg;
		char id[96];

		pjsua_acc_config_default(&acc_cfg);
		pj_ansi_snprintf(id, sizeof(id), "<sip:capacity0@127.0.0.1>");
		acc_cfg.id = pj_str(id);
		account_sentinels[0] = 0x5100;
		acc_cfg.user_data = &account_sentinels[0];
		status = pjsua_acc_add(&acc_cfg, PJ_FALSE, &account_ids[0]);
		CHECK(status == PJ_SUCCESS, "warm-up account add");
		if (status != PJ_SUCCESS)
			goto cleanup;
		CHECK(pjsua_acc_set_default(account_ids[0]) == PJ_SUCCESS,
		      "warm-up default account");
	}
	CHECK(warmup_busy(&peers[0], transport_info.local_name.port) == 0,
	      "warm up busy response");
	CHECK(pjsua_acc_del(account_ids[0]) == PJ_SUCCESS,
	      "warm-up account delete");
	account_ids[0] = PJSUA_INVALID_ID;
	atomic_set(&incoming_count, 0);
	atomic_set(&callback_total, 0);
	CHECK(pjsua_call_get_count() == 0, "warm-up leaves zero calls");
	CHECK(wait_for_cleanup_quiescence(transport_baseline) == 0,
	      "warm-up SIP state quiesces");
	for (i = 0; i < EVENT_POLL_LIMIT; ++i)
		(void)pjsua_handle_events(10);
	pj_zephyr_pool_arena_get_stats(&baseline);
	baseline_valid = true;

	for (i = 0; i < ACCOUNT_COUNT; ++i) {
		pjsua_acc_config acc_cfg;
		char id[96];

		pjsua_acc_config_default(&acc_cfg);
		pj_ansi_snprintf(id, sizeof(id), "<sip:capacity%u@127.0.0.1>", i);
		acc_cfg.id = pj_str(id);
		account_sentinels[i] = (int)(0x5100 + i);
		acc_cfg.user_data = &account_sentinels[i];
		status = pjsua_acc_add(&acc_cfg, PJ_FALSE, &account_ids[i]);
		CHECK(status == PJ_SUCCESS, "five account records add");
		if (status != PJ_SUCCESS)
			goto cleanup;
	}
	status = pjsua_acc_set_default(account_ids[0]);
	CHECK(status == PJ_SUCCESS, "default account");

	count = ACCOUNT_COUNT;
	{
		pjsua_acc_id enumerated[ACCOUNT_COUNT];
		status = pjsua_enum_accs(enumerated, &count);
		CHECK(status == PJ_SUCCESS && count == ACCOUNT_COUNT,
		      "enumerate five accounts");
		for (i = 0; i < count; ++i)
			CHECK(pjsua_acc_get_user_data(enumerated[i]) ==
			      &account_sentinels[enumerated[i]],
			      "account user-data sentinel");
	}

	for (i = 0; i < HELD_CALL_COUNT; ++i) {
		if (i == 0)
			status = connect_peer(&peers[0],
					      transport_info.local_name.port, i + 1);
		else
			status = send_invite(peers[0], transport_info.local_name.port,
					     i + 1);
		if (i == 0 && status == PJ_SUCCESS)
			status = send_invite(peers[0], transport_info.local_name.port,
					     i + 1);
		CHECK(status == PJ_SUCCESS, "incoming TCP INVITE");
		if (failures != 0)
			goto cleanup;
		CHECK(poll_incoming(i + 1) == 0, "hold incoming call record");
		if (failures != 0)
			goto cleanup;
	}

	CHECK((unsigned)pjsua_call_get_count() == HELD_CALL_COUNT,
	      "seven calls held");
	verify_held_calls(true);

	CHECK(send_invite(peers[0], transport_info.local_name.port,
			  HELD_CALL_COUNT + 1) == PJ_SUCCESS, "eighth TCP INVITE");
	CHECK(response_is_busy(peers[0]) == 0,
	      "eighth INVITE receives SIP 486 Busy Here");
	CHECK((unsigned)atomic_get(&callback_total) == HELD_CALL_COUNT,
	      "eighth INVITE does not invoke incoming callback");
	CHECK((unsigned)pjsua_call_get_count() == HELD_CALL_COUNT,
	      "eighth INVITE leaves seven calls held");
	verify_held_calls(false);

	for (i = 0; i < HELD_CALL_COUNT; ++i) {
		status = pjsua_call_hangup(held_call_ids[i], PJSIP_SC_DECLINE,
					   NULL, NULL);
		CHECK(status == PJ_SUCCESS, "hang up held call");
	}
	CHECK(acknowledge_declines(peers[0], transport_info.local_name.port) == 0,
	      "acknowledge seven final responses");
	(void)pj_sock_close(peers[0]);
	peers[0] = PJ_INVALID_SOCKET;
	for (i = 0; i < EVENT_POLL_LIMIT && pjsua_call_get_count() != 0; ++i)
		(void)pjsua_handle_events(10);
	CHECK(pjsua_call_get_count() == 0, "all seven calls destroyed");
	CHECK(wait_for_cleanup_quiescence(transport_baseline) == 0,
	      "TCP transport and SIP state quiesce");

cleanup:
	for (i = 0; i < ACCOUNT_COUNT; ++i) {
		if (pjsua_acc_is_valid(account_ids[i]))
			CHECK(pjsua_acc_del(account_ids[i]) == PJ_SUCCESS,
			      "destroy account record");
	}
	if (baseline_valid) {
		CHECK(wait_for_cleanup_quiescence(transport_baseline) == 0,
		      "SIP quiescence after account cleanup");
		for (i = 0; i < EVENT_POLL_LIMIT; ++i)
			(void)pjsua_handle_events(10);
		pj_zephyr_pool_arena_get_stats(&cleanup);
		CHECK(cleanup.used_bytes == baseline.used_bytes &&
		      cleanup.live_blocks == baseline.live_blocks,
		      "arena returns to post-start baseline before destroy");
		printk("PJSUA CAPACITY CLEANUP: baseline used=%u live=%u cleanup used=%u live=%u\n",
		       (unsigned)baseline.used_bytes, (unsigned)baseline.live_blocks,
		       (unsigned)cleanup.used_bytes, (unsigned)cleanup.live_blocks);
	}

destroy:
	for (i = 0; i < HELD_CALL_COUNT + 1; ++i) {
		if (peers[i] != PJ_INVALID_SOCKET)
			(void)pj_sock_close(peers[i]);
	}
	if (pjsua_get_state() != PJSUA_STATE_NULL)
		CHECK(pjsua_destroy() == PJ_SUCCESS, "pjsua destroy");
	pj_zephyr_pool_arena_get_stats(&destroyed);
	CHECK(destroyed.used_bytes == 0 && destroyed.live_blocks == 0,
	      "arena empty after pjsua destroy");
	printk("PJSUA CAPACITY ARENA: destroyed used=%u live=%u peak=%u\n",
	       (unsigned)destroyed.used_bytes, (unsigned)destroyed.live_blocks,
	       (unsigned)destroyed.peak_bytes);
	return failures == 0 ? 0 : -1;
}

int main(void)
{
	const int result = run_capacity_test();

	printk("PJSUA CAPACITY RESULT: %s\n",
	       result == 0 ? "PASSED (5 accounts, 7 calls, eighth 486)" :
	       "FAILED");
	return result;
}
