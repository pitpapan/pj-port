#include <pjsip.h>
#include <pjsip/sip_transport_udp.h>
#include <pjsip-ua/sip_regc.h>
#include <pjlib-util.h>
#include <pjlib.h>

#include <errno.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PHASE10_LIFECYCLES 2
#define PHASE10_WAIT_MS 2500
#define PHASE10_REFRESH_WAIT_MS 7000
#define PHASE10_REG_EXPIRES 6

_Static_assert(PJ_HAS_IPV6 == 0,
	       "Phase 10 intentionally validates the IPv4 signaling profile");

struct phase10_context;

struct phase10_reg_result {
	struct phase10_context *context;
	atomic_t callback_count;
	atomic_t status;
	atomic_t code;
	atomic_t expiration;
	atomic_t is_unreg;
};

struct phase10_options_result {
	struct phase10_context *context;
	atomic_t called;
	atomic_t code;
};

struct phase10_context {
	pjsip_endpoint *endpt;
	pjsip_transport *server_udp;
	pjsip_transport *client_udp;
	pjsip_regc *active_regc;
	pjsip_auth_srv auth_srv;
	pj_thread_t *event_thread;
	pjsip_tp_state_callback previous_state_cb;

	atomic_t event_started;
	atomic_t event_stop;
	atomic_t event_error;
	atomic_t event_polls;
	atomic_t teardown_started;
	atomic_t teardown_poll_retries;
	atomic_t callback_error;
	atomic_t endpoint_exit_count;
	atomic_t transport_shutdowns;
	atomic_t transport_destroys;

	atomic_t drop_recovery;
	atomic_t register_requests;
	atomic_t challenges;
	atomic_t authorized_requests;
	atomic_t invalid_authorizations;
	atomic_t main_registrations;
	atomic_t main_unregistrations;
	atomic_t invalid_requests;
	atomic_t recovery_requests;
	atomic_t recovery_registrations;
	atomic_t options_requests;
};

enum phase10_registration {
	PHASE10_REG_MAIN,
	PHASE10_REG_INVALID,
	PHASE10_REG_RECOVERY,
	PHASE10_REG_UNKNOWN,
};

static struct phase10_context *active_context;

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 10] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
	return -1;
}

static int fail_value(const char *test, int line, const char *condition)
{
	printk("[Phase 10] FAIL %s:%d condition=%s\n",
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

static void record_callback_error(struct phase10_context *context, int error)
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

static int wait_for_count(struct phase10_context *context, atomic_t *value,
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

static int wait_for_transactions(struct phase10_context *context,
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

static int wait_for_timers(struct phase10_context *context, unsigned expected,
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

static int phase10_event_thread(void *arg)
{
	struct phase10_context *context = arg;

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

static enum phase10_registration registration_from_request(
	const pjsip_rx_data *rdata)
{
	const pjsip_from_hdr *from = rdata->msg_info.from;
	const pjsip_sip_uri *uri;

	if (from == NULL || from->uri == NULL ||
	    (!PJSIP_URI_SCHEME_IS_SIP(from->uri) &&
	     !PJSIP_URI_SCHEME_IS_SIPS(from->uri)))
		return PHASE10_REG_UNKNOWN;
	uri = (const pjsip_sip_uri *)pjsip_uri_get_uri(from->uri);
	if (pj_strcmp2(&uri->user, "main") == 0)
		return PHASE10_REG_MAIN;
	if (pj_strcmp2(&uri->user, "invalid") == 0)
		return PHASE10_REG_INVALID;
	if (pj_strcmp2(&uri->user, "recovery") == 0)
		return PHASE10_REG_RECOVERY;
	return PHASE10_REG_UNKNOWN;
}

static unsigned request_expiration(const pjsip_rx_data *rdata)
{
	const pjsip_expires_hdr *expires;
	const pjsip_contact_hdr *contact;

	expires = (const pjsip_expires_hdr *)pjsip_msg_find_hdr(
		rdata->msg_info.msg, PJSIP_H_EXPIRES, NULL);
	if (expires != NULL)
		return expires->ivalue;
	contact = (const pjsip_contact_hdr *)pjsip_msg_find_hdr(
		rdata->msg_info.msg, PJSIP_H_CONTACT, NULL);
	if (contact != NULL && contact->expires != PJSIP_EXPIRES_NOT_SPECIFIED)
		return (unsigned)contact->expires;
	return PJSIP_REGC_EXPIRATION_NOT_SPECIFIED;
}

static pj_status_t phase10_lookup_credential(pj_pool_t *pool,
					      const pj_str_t *realm,
					      const pj_str_t *account,
					      pjsip_cred_info *credential)
{
	PJ_UNUSED_ARG(pool);
	if (pj_strcmp2(account, "alice") != 0)
		return PJSIP_EAUTHACCNOTFOUND;
	pj_bzero(credential, sizeof(*credential));
	credential->realm = *realm;
	credential->scheme = pj_str("digest");
	credential->username = *account;
	credential->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
	credential->data = pj_str("phase10-secret");
	return PJ_SUCCESS;
}

static pj_bool_t phase10_on_rx_request(pjsip_rx_data *rdata);

static pjsip_module phase10_module = {
	.name = {"phase10-registrar", 17},
	.id = -1,
	.priority = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_rx_request = phase10_on_rx_request,
};

static pj_status_t send_local_response(struct phase10_context *context,
				       pjsip_rx_data *rdata, int code,
				       pj_bool_t challenge,
				       unsigned expiration)
{
	pjsip_transaction *uas = NULL;
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_tsx_create_uas(&phase10_module, rdata, &uas);
	if (status != PJ_SUCCESS)
		return status;
	pjsip_tsx_recv_msg(uas, rdata);
	status = pjsip_endpt_create_response(context->endpt, rdata, code, NULL,
					    &tdata);
	if (status != PJ_SUCCESS) {
		pjsip_tsx_terminate(uas, 500);
		return status;
	}
	if (challenge) {
		pj_str_t qop = pj_str("auth");
		pj_str_t nonce = pj_str("phase10-deterministic-nonce");
		pj_str_t opaque = pj_str("phase10-opaque");

		status = pjsip_auth_srv_challenge(&context->auth_srv, &qop,
						  &nonce, &opaque, PJ_FALSE,
						  tdata);
	}
	if (status == PJ_SUCCESS && code / 100 == 2 &&
	    rdata->msg_info.msg->line.req.method.id == PJSIP_REGISTER_METHOD) {
		const pjsip_contact_hdr *request_contact;
		pjsip_expires_hdr *response_expires;

		response_expires = pjsip_expires_hdr_create(tdata->pool,
							       expiration);
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *)response_expires);
		request_contact = (const pjsip_contact_hdr *)pjsip_msg_find_hdr(
			rdata->msg_info.msg, PJSIP_H_CONTACT, NULL);
		if (request_contact != NULL) {
			pjsip_contact_hdr *response_contact;

			response_contact = (pjsip_contact_hdr *)pjsip_hdr_clone(
				tdata->pool, (const pjsip_hdr *)request_contact);
			response_contact->expires = (int)expiration;
			pjsip_msg_add_hdr(tdata->msg,
					  (pjsip_hdr *)response_contact);
		}
	}
	if (status == PJ_SUCCESS)
		status = pjsip_tsx_send_msg(uas, tdata);
	if (status != PJ_SUCCESS) {
		if (tdata != NULL)
			pjsip_tx_data_dec_ref(tdata);
		pjsip_tsx_terminate(uas, 500);
	}
	return status;
}

static pj_bool_t phase10_on_rx_request(pjsip_rx_data *rdata)
{
	struct phase10_context *context = active_context;
	pjsip_msg *message = rdata->msg_info.msg;
	pj_status_t status;

	if (context == NULL || message == NULL ||
	    message->type != PJSIP_REQUEST_MSG)
		return PJ_FALSE;
	if (rdata->tp_info.transport != context->server_udp) {
		record_callback_error(context, -401);
		return PJ_TRUE;
	}
	if (message->line.req.method.id == PJSIP_OPTIONS_METHOD) {
		atomic_inc(&context->options_requests);
		status = send_local_response(context, rdata, 200, PJ_FALSE, 0);
		if (status != PJ_SUCCESS)
			record_callback_error(context, status);
		return PJ_TRUE;
	}
	if (message->line.req.method.id == PJSIP_REGISTER_METHOD) {
		enum phase10_registration registration;
		unsigned expiration;
		int response_code = 200;

		registration = registration_from_request(rdata);
		atomic_inc(&context->register_requests);
		if (registration == PHASE10_REG_INVALID)
			atomic_inc(&context->invalid_requests);
		else if (registration == PHASE10_REG_RECOVERY)
			atomic_inc(&context->recovery_requests);
		else if (registration != PHASE10_REG_MAIN) {
			record_callback_error(context, -402);
			return PJ_TRUE;
		}
		if (registration == PHASE10_REG_RECOVERY &&
		    atomic_get(&context->drop_recovery) != 0)
			return PJ_TRUE;

		status = pjsip_auth_srv_verify(&context->auth_srv, rdata,
					       &response_code);
		if (status == PJSIP_EAUTHNOAUTH) {
			atomic_inc(&context->challenges);
			status = send_local_response(context, rdata, 401, PJ_TRUE, 0);
		} else if (status != PJ_SUCCESS) {
			atomic_inc(&context->invalid_authorizations);
			status = send_local_response(context, rdata, response_code,
						     PJ_FALSE, 0);
		} else {
			expiration = request_expiration(rdata);
			if (expiration == PJSIP_REGC_EXPIRATION_NOT_SPECIFIED)
				expiration = PHASE10_REG_EXPIRES;
			atomic_inc(&context->authorized_requests);
			if (registration == PHASE10_REG_MAIN) {
				if (expiration == 0)
					atomic_inc(&context->main_unregistrations);
				else
					atomic_inc(&context->main_registrations);
			} else if (registration == PHASE10_REG_RECOVERY &&
				   expiration != 0) {
				atomic_inc(&context->recovery_registrations);
			}
			status = send_local_response(context, rdata, 200, PJ_FALSE,
						     expiration);
		}
		if (status != PJ_SUCCESS)
			record_callback_error(context, status);
		return PJ_TRUE;
	}
	return PJ_FALSE;
}

static void phase10_regc_cb(struct pjsip_regc_cbparam *param)
{
	struct phase10_reg_result *result = param->token;

	if (result == NULL || result->context == NULL) {
		if (active_context != NULL)
			record_callback_error(active_context, -410);
		return;
	}
	atomic_set(&result->status, param->status);
	atomic_set(&result->code, param->code);
	atomic_set(&result->expiration, (atomic_val_t)param->expiration);
	atomic_set(&result->is_unreg, param->is_unreg);
	atomic_inc(&result->callback_count);
}

static void phase10_options_cb(void *token, pjsip_event *event)
{
	struct phase10_options_result *result = token;
	pjsip_transaction *transaction;

	if (result == NULL || result->context == NULL || event == NULL ||
	    event->type != PJSIP_EVENT_TSX_STATE) {
		if (active_context != NULL)
			record_callback_error(active_context, -411);
		return;
	}
	transaction = event->body.tsx_state.tsx;
	atomic_set(&result->code, transaction->status_code);
	if (atomic_inc(&result->called) != 0)
		record_callback_error(result->context, -412);
}

static pj_status_t create_regc(struct phase10_context *context,
			       struct phase10_reg_result *result,
			       const char *user, const char *password,
			       unsigned expires, pjsip_regc **registration)
{
	char registrar_text[96];
	char identity_text[96];
	char contact_text[112];
	pj_str_t registrar;
	pj_str_t identity;
	pj_str_t contact;
	pjsip_cred_info credential;
	pjsip_tpselector selector;
	pj_status_t status;

	if (pj_ansi_snprintf(registrar_text, sizeof(registrar_text),
			     "sip:127.0.0.1:%u;transport=udp",
			     context->server_udp->local_name.port) <= 0 ||
	    pj_ansi_snprintf(identity_text, sizeof(identity_text),
			     "<sip:%s@phase10.test>", user) <= 0 ||
	    pj_ansi_snprintf(contact_text, sizeof(contact_text),
			     "<sip:%s@127.0.0.1:%u;transport=udp>", user,
			     context->client_udp->local_name.port) <= 0)
		return PJ_ETOOSMALL;
	registrar = pj_str(registrar_text);
	identity = pj_str(identity_text);
	contact = pj_str(contact_text);
	pj_bzero(result, sizeof(*result));
	result->context = context;
	status = pjsip_regc_create(context->endpt, result, phase10_regc_cb,
				  registration);
	if (status != PJ_SUCCESS)
		return status;
	context->active_regc = *registration;
	status = pjsip_regc_init(*registration, &registrar, &identity, &identity,
				1, &contact, expires);
	if (status != PJ_SUCCESS)
		return status;
	pj_bzero(&credential, sizeof(credential));
	credential.realm = pj_str("*");
	credential.scheme = pj_str("digest");
	credential.username = pj_str("alice");
	credential.data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
	credential.data = pj_str((char *)password);
	status = pjsip_regc_set_credentials(*registration, 1, &credential);
	if (status != PJ_SUCCESS)
		return status;
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	return pjsip_regc_set_transport(*registration, &selector);
}

static pj_status_t send_registration(pjsip_regc *registration,
				     pj_bool_t auto_refresh)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_regc_register(registration, auto_refresh, &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_send(registration, tdata);
	return status;
}

static pj_status_t send_unregistration(pjsip_regc *registration)
{
	pjsip_tx_data *tdata = NULL;
	pj_status_t status;

	status = pjsip_regc_unregister(registration, &tdata);
	if (status == PJ_SUCCESS)
		status = pjsip_regc_send(registration, tdata);
	return status;
}

static pj_status_t destroy_active_regc(struct phase10_context *context)
{
	pj_status_t status;

	if (context->active_regc == NULL)
		return PJ_SUCCESS;
	status = pjsip_regc_destroy2(context->active_regc, PJ_TRUE);
	context->active_regc = NULL;
	return status;
}

static int test_registration_profile(struct phase10_context *context)
{
	const char *test = "authenticated registration profile";
	struct phase10_reg_result result;
	pjsip_regc *registration = NULL;
	pjsip_regc_info info;
	atomic_val_t callbacks;

	CHECK_STATUS(test, create_regc(context, &result, "main",
				       "phase10-secret", PHASE10_REG_EXPIRES,
				       &registration));
	CHECK_STATUS(test, pjsip_regc_set_delay_before_refresh(registration, 1));
	CHECK_STATUS(test, send_registration(registration, PJ_TRUE));
	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 1,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, atomic_get(&result.code) == 200);
	CHECK_TRUE(test, atomic_get(&result.expiration) == PHASE10_REG_EXPIRES);
	CHECK_TRUE(test, atomic_get(&context->main_registrations) == 1);
	CHECK_TRUE(test, atomic_get(&context->challenges) >= 1);
	CHECK_STATUS(test, pjsip_regc_get_info(registration, &info));
	CHECK_TRUE(test, info.auto_reg == PJ_TRUE);
	CHECK_TRUE(test, info.interval == PHASE10_REG_EXPIRES);
	CHECK_TRUE(test, info.next_reg > 0 && info.next_reg <= 5);
	printk("[Phase 10] REGISTER -> 401 Digest challenge -> authenticated 200 OK: PASSED\n");

	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 2,
				       PHASE10_REFRESH_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.code) == 200);
	CHECK_TRUE(test, atomic_get(&context->main_registrations) == 2);
	printk("[Phase 10] automatic registration refresh before six-second expiry: PASSED\n");

	CHECK_STATUS(test, send_unregistration(registration));
	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 3,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.code) == 200);
	CHECK_TRUE(test, atomic_get(&result.expiration) == 0);
	CHECK_TRUE(test, atomic_get(&result.is_unreg) == PJ_TRUE);
	CHECK_TRUE(test, atomic_get(&context->main_unregistrations) == 1);
	callbacks = atomic_get(&result.callback_count);
	CHECK_STATUS(test, destroy_active_regc(context));
	registration = NULL;
	pj_thread_sleep(200);
	CHECK_TRUE(test, atomic_get(&result.callback_count) == callbacks);
	printk("[Phase 10] authenticated unregister canceled refresh and returned 200 OK: PASSED\n");
	return 0;
}

static int test_invalid_credentials(struct phase10_context *context)
{
	const char *test = "invalid registration credentials";
	struct phase10_reg_result result;
	pjsip_regc *registration = NULL;
	atomic_val_t invalid_before =
		atomic_get(&context->invalid_authorizations);

	CHECK_STATUS(test, create_regc(context, &result, "invalid",
				       "wrong-password", 30, &registration));
	CHECK_STATUS(test, send_registration(registration, PJ_FALSE));
	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 1,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, atomic_get(&result.code) == 403);
	CHECK_TRUE(test, atomic_get(&context->invalid_authorizations) ==
			 invalid_before + 1);
	CHECK_STATUS(test, destroy_active_regc(context));
	registration = NULL;
	printk("[Phase 10] invalid Digest credentials rejected with 403: PASSED\n");
	return 0;
}

static int test_timeout_recovery(struct phase10_context *context)
{
	const char *test = "registrar timeout and recovery";
	struct phase10_reg_result result;
	pjsip_regc *registration = NULL;

	atomic_set(&context->drop_recovery, 1);
	CHECK_STATUS(test, create_regc(context, &result, "recovery",
				       "phase10-secret", 30, &registration));
	CHECK_STATUS(test, send_registration(registration, PJ_FALSE));
	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 1,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.code) == 408);
	CHECK_TRUE(test, atomic_get(&context->recovery_requests) >= 2);
	atomic_set(&context->drop_recovery, 0);
	CHECK_STATUS(test, send_registration(registration, PJ_FALSE));
	CHECK_TRUE(test, wait_for_count(context, &result.callback_count, 2,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.status) == PJ_SUCCESS);
	CHECK_TRUE(test, atomic_get(&result.code) == 200);
	CHECK_TRUE(test, atomic_get(&context->recovery_registrations) == 1);
	CHECK_STATUS(test, destroy_active_regc(context));
	registration = NULL;
	printk("[Phase 10] registrar timeout retransmitted, returned 408, then recovered to 200: PASSED\n");
	return 0;
}

static int test_options(struct phase10_context *context)
{
	const char *test = "application OPTIONS exchange";
	struct phase10_options_result result;
	char target_text[96];
	char source_text[96];
	pj_str_t target;
	pj_str_t source;
	pj_str_t call_id = pj_str("phase10-options");
	pjsip_tx_data *tdata = NULL;
	pjsip_tpselector selector;
	pj_status_t status;

	pj_bzero(&result, sizeof(result));
	result.context = context;
	CHECK_TRUE(test, pj_ansi_snprintf(target_text, sizeof(target_text),
					 "sip:registrar@127.0.0.1:%u;transport=udp",
					 context->server_udp->local_name.port) > 0);
	CHECK_TRUE(test, pj_ansi_snprintf(source_text, sizeof(source_text),
					 "<sip:main@127.0.0.1:%u>",
					 context->client_udp->local_name.port) > 0);
	target = pj_str(target_text);
	source = pj_str(source_text);
	CHECK_STATUS(test, pjsip_endpt_create_request(
		context->endpt, &pjsip_options_method, &target, &source, &target,
		NULL, &call_id, 1000, NULL, &tdata));
	pj_bzero(&selector, sizeof(selector));
	selector.type = PJSIP_TPSELECTOR_TRANSPORT;
	selector.u.transport = context->client_udp;
	status = pjsip_tx_data_set_transport(tdata, &selector);
	if (status == PJ_SUCCESS)
		status = pjsip_endpt_send_request(context->endpt, tdata, -1,
						  &result, phase10_options_cb);
	if (status != PJ_SUCCESS) {
		pjsip_tx_data_dec_ref(tdata);
		return fail_status(test, __LINE__, status);
	}
	CHECK_TRUE(test, wait_for_count(context, &result.called, 1,
				       PHASE10_WAIT_MS) == 0);
	CHECK_TRUE(test, atomic_get(&result.code) == 200);
	CHECK_TRUE(test, atomic_get(&context->options_requests) == 1);
	printk("[Phase 10] application-level OPTIONS request/200 response over UDP: PASSED\n");
	return 0;
}

static void phase10_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	if (active_context != NULL)
		atomic_inc(&active_context->endpoint_exit_count);
}

static void phase10_on_transport_state(
	pjsip_transport *transport, pjsip_transport_state state,
	const pjsip_transport_state_info *info)
{
	struct phase10_context *context = active_context;
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
	if (previous != NULL && previous != phase10_on_transport_state)
		previous(transport, state, info);
}

static int run_lifecycle(int iteration)
{
	struct phase10_context context;
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *thread_pool = NULL;
	pj_pool_t *test_pool = NULL;
	pjsip_transport *server_udp = NULL;
	pjsip_transport *client_udp = NULL;
	pjsip_tpmgr *transport_manager = NULL;
	pj_sockaddr_in bind_address;
	pj_str_t loopback = pj_str("127.0.0.1");
	pj_str_t realm = pj_str("phase10");
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
	pj_log_set_level(3);
	status = pjlib_util_init();
	if (status != PJ_SUCCESS) {
		fail_status("pjlib_util_init", __LINE__, status);
		goto shutdown;
	}
	pj_caching_pool_init(&caching_pool, NULL, 0);
	caching_pool_initialized = PJ_TRUE;
	status = pjsip_endpt_create(&caching_pool.factory, "phase10", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	context.endpt = endpoint;
	active_context = &context;
	status = pjsip_endpt_atexit(endpoint, phase10_endpoint_exit);
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
	status = pjsip_endpt_register_module(endpoint, &phase10_module);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_register_module", __LINE__, status);
		goto destroy_endpoint;
	}
	module_registered = PJ_TRUE;
	transport_manager = pjsip_endpt_get_tpmgr(endpoint);
	context.previous_state_cb = pjsip_tpmgr_get_state_cb(transport_manager);
	status = pjsip_tpmgr_set_state_cb(transport_manager,
					 phase10_on_transport_state);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_tpmgr_set_state_cb", __LINE__, status);
		goto destroy_endpoint;
	}
	callbacks_installed = PJ_TRUE;

	status = pj_sockaddr_in_init(&bind_address, &loopback, 0);
	if (status == PJ_SUCCESS)
		status = pjsip_udp_transport_start(endpoint, &bind_address, NULL, 1,
						   &server_udp);
	if (status != PJ_SUCCESS) {
		fail_status("registrar UDP transport", __LINE__, status);
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

	test_pool = pjsip_endpt_create_pool(endpoint, "phase10-test", 8192, 4096);
	thread_pool = pjsip_endpt_create_pool(endpoint, "phase10-thread", 8192,
					      4096);
	if (test_pool == NULL || thread_pool == NULL) {
		fail_value("Phase 10 pools", __LINE__, "test and thread pools exist");
		goto destroy_endpoint;
	}
	status = pjsip_auth_srv_init(test_pool, &context.auth_srv, &realm,
				     phase10_lookup_credential, 0);
	if (status != PJ_SUCCESS) {
		fail_status("local registrar authentication", __LINE__, status);
		goto destroy_endpoint;
	}
	status = pj_thread_create(thread_pool, "p10-event", phase10_event_thread,
				  &context, PJ_THREAD_DEFAULT_STACK_SIZE, 0,
				  &context.event_thread);
	if (status != PJ_SUCCESS) {
		fail_status("Phase 10 event thread", __LINE__, status);
		goto destroy_endpoint;
	}
	if (wait_for_count(&context, &context.event_started, 1,
			   PHASE10_WAIT_MS) != 0) {
		fail_value("Phase 10 event thread", __LINE__, "event thread started");
		goto destroy_endpoint;
	}
	printk("[Phase 10] client UDP %u and deterministic local registrar UDP %u: PASSED\n",
	       client_udp->local_name.port, server_udp->local_name.port);

	if (test_registration_profile(&context) != 0 ||
	    test_invalid_credentials(&context) != 0 ||
	    test_timeout_recovery(&context) != 0 ||
	    test_options(&context) != 0)
		goto destroy_endpoint;
	result = 0;

destroy_endpoint:
	atomic_set(&context.teardown_started, 1);
	if (context.active_regc != NULL) {
		status = destroy_active_regc(&context);
		if (status != PJ_SUCCESS) {
			fail_status("registration client destruction", __LINE__, status);
			result = -1;
		}
	}
	if (context.event_thread != NULL &&
	    (wait_for_transactions(&context, 0, PHASE10_WAIT_MS) != 0 ||
	     wait_for_timers(&context, 0, PHASE10_WAIT_MS) != 0)) {
		fail_value("signaling teardown", __LINE__,
			   "transactions and registration timers drained");
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
			fail_status("registrar UDP shutdown", __LINE__, status);
			result = -1;
		}
		server_udp_started = PJ_FALSE;
	}
	if (context.event_thread != NULL &&
	    wait_for_count(&context, &context.transport_destroys, 2,
			   PHASE10_WAIT_MS) != 0) {
		fail_value("UDP transport teardown", __LINE__,
			   "client and registrar transports destroyed");
		result = -1;
	}
	context.client_udp = NULL;
	context.server_udp = NULL;
	client_udp = NULL;
	server_udp = NULL;
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
		status = pjsip_endpt_unregister_module(endpoint, &phase10_module);
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
		printk("[Phase 10] lifecycle %d regc/transports/transactions/timers/endpoint teardown: PASSED (select close-race retries=%d)\n",
		       iteration,
		       (int)atomic_get(&context.teardown_poll_retries));

destroy_factory:
	active_context = NULL;
	if (caching_pool_initialized)
		pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 10] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase10_signaling_run(void)
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
	printk("[Phase 10] usable SIP registration and signaling validation (%d lifecycles)\n",
	       PHASE10_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE10_LIFECYCLES; ++iteration) {
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
		printk("PHASE 10 RESULT: PASSED (%d/%d lifecycles)\n",
		       PHASE10_LIFECYCLES, PHASE10_LIFECYCLES);
	else
		printk("PHASE 10 RESULT: FAILED at lifecycle %d\n", iteration);
	return result == 0 ? 0 : 1;
}
