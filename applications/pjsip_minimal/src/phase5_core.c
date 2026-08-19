#include <pjsip.h>
#include <pjlib-util.h>

#include <string.h>

#include <zephyr/sys/printk.h>

#define PHASE5_LIFECYCLES 3

static int fail_status(const char *test, int line, pj_status_t status)
{
	char text[PJ_ERR_MSG_SIZE];

	pj_strerror(status, text, sizeof(text));
	printk("[Phase 5] FAIL %s:%d status=%d (%s)\n",
	       test, line, status, text);
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

static char *pool_copy_text(pj_pool_t *pool, const char *text)
{
	pj_size_t length = strlen(text);
	char *copy = pj_pool_alloc(pool, length + 1);

	pj_memcpy(copy, text, length + 1);
	return copy;
}

static pj_bool_t string_equals(const pj_str_t *value, const char *expected)
{
	pj_size_t length = strlen(expected);

	return value->slen == (pj_ssize_t)length &&
	       pj_memcmp(value->ptr, expected, length) == 0;
}

static pj_bool_t string_starts_with(const pj_str_t *value, const char *prefix)
{
	pj_size_t length = strlen(prefix);

	return value->slen >= (pj_ssize_t)length &&
	       pj_memcmp(value->ptr, prefix, length) == 0;
}

static int test_uri_parser(pj_pool_t *pool)
{
	const char *test = "URI parser";
	static const struct {
		const char *text;
		pj_bool_t secure;
	} valid[] = {
		{"sip:alice@example.com", PJ_FALSE},
		{"sips:alice:secret@example.com:5061;transport=tcp;lr", PJ_TRUE},
		{"\"Alice Smith\" <sip:alice%20smith@example.com;user=phone>", PJ_FALSE},
		{"sip:service@example.com;transport=udp;lr;maddr=127.0.0.1"
		 "?Subject=Hello%20There", PJ_FALSE},
	};
	static const char *const malformed[] = {
		"sip:",
		"sip :alice@example.com",
		"sip:alice@",
		"sip:alice@example.com:bad",
		" \"Rogue User\\\" <sip:localhost>",
		"",
	};
	unsigned i;

	for (i = 0; i < PJ_ARRAY_SIZE(valid); ++i) {
		pjsip_uri *uri;
		pjsip_uri *reparsed;
		void *base_uri;
		char *input = pool_copy_text(pool, valid[i].text);
		char *printed = pj_pool_alloc(pool, PJSIP_MAX_URL_SIZE + 1);
		int printed_length;

		uri = pjsip_parse_uri(pool, input, strlen(input), 0);
		CHECK_TRUE(test, uri != NULL);
		base_uri = pjsip_uri_get_uri(uri);
		CHECK_TRUE(test, valid[i].secure ? PJSIP_URI_SCHEME_IS_SIPS(base_uri) :
					       PJSIP_URI_SCHEME_IS_SIP(base_uri));

		printed_length = pjsip_uri_print(PJSIP_URI_IN_OTHER, uri, printed,
					 PJSIP_MAX_URL_SIZE);
		CHECK_TRUE(test, printed_length > 0 &&
				 printed_length <= PJSIP_MAX_URL_SIZE);
		printed[printed_length] = '\0';
		reparsed = pjsip_parse_uri(pool, printed, printed_length, 0);
		CHECK_TRUE(test, reparsed != NULL);
		CHECK_TRUE(test, pjsip_uri_cmp(PJSIP_URI_IN_OTHER, uri, reparsed) ==
				 PJ_SUCCESS);
	}

	for (i = 0; i < PJ_ARRAY_SIZE(malformed); ++i) {
		char *input = pool_copy_text(pool, malformed[i]);

		CHECK_TRUE(test, pjsip_parse_uri(pool, input, strlen(input), 0) == NULL);
	}

	printk("[Phase 5] SIP/SIPS URI parse/round-trip/rejection: PASSED\n");
	return 0;
}

static int compare_message_semantics(const pjsip_msg *first,
				     const pjsip_msg *second)
{
	const char *test = "message semantic round-trip";
	pjsip_from_hdr *first_from;
	pjsip_from_hdr *second_from;
	pjsip_to_hdr *first_to;
	pjsip_to_hdr *second_to;
	pjsip_cid_hdr *first_cid;
	pjsip_cid_hdr *second_cid;
	pjsip_cseq_hdr *first_cseq;
	pjsip_cseq_hdr *second_cseq;

	CHECK_TRUE(test, first->type == second->type);
	if (first->type == PJSIP_REQUEST_MSG) {
		CHECK_TRUE(test, pjsip_method_cmp(&first->line.req.method,
						 &second->line.req.method) == 0);
		CHECK_TRUE(test, pjsip_uri_cmp(PJSIP_URI_IN_REQ_URI,
						 first->line.req.uri,
						 second->line.req.uri) == PJ_SUCCESS);
	} else {
		CHECK_TRUE(test, first->line.status.code == second->line.status.code);
		CHECK_TRUE(test, pj_strcmp(&first->line.status.reason,
					   &second->line.status.reason) == 0);
	}

	first_from = (pjsip_from_hdr *)pjsip_msg_find_hdr(first, PJSIP_H_FROM, NULL);
	second_from = (pjsip_from_hdr *)pjsip_msg_find_hdr(second, PJSIP_H_FROM, NULL);
	first_to = (pjsip_to_hdr *)pjsip_msg_find_hdr(first, PJSIP_H_TO, NULL);
	second_to = (pjsip_to_hdr *)pjsip_msg_find_hdr(second, PJSIP_H_TO, NULL);
	first_cid = (pjsip_cid_hdr *)pjsip_msg_find_hdr(first, PJSIP_H_CALL_ID, NULL);
	second_cid = (pjsip_cid_hdr *)pjsip_msg_find_hdr(second, PJSIP_H_CALL_ID, NULL);
	first_cseq = (pjsip_cseq_hdr *)pjsip_msg_find_hdr(first, PJSIP_H_CSEQ, NULL);
	second_cseq = (pjsip_cseq_hdr *)pjsip_msg_find_hdr(second, PJSIP_H_CSEQ, NULL);
	CHECK_TRUE(test, first_from && second_from && first_to && second_to &&
			 first_cid && second_cid && first_cseq && second_cseq);
	CHECK_TRUE(test, pjsip_uri_cmp(PJSIP_URI_IN_FROMTO_HDR, first_from->uri,
				 second_from->uri) == PJ_SUCCESS);
	CHECK_TRUE(test, pjsip_uri_cmp(PJSIP_URI_IN_FROMTO_HDR, first_to->uri,
				 second_to->uri) == PJ_SUCCESS);
	CHECK_TRUE(test, pj_strcmp(&first_from->tag, &second_from->tag) == 0);
	CHECK_TRUE(test, pj_strcmp(&first_to->tag, &second_to->tag) == 0);
	CHECK_TRUE(test, pj_strcmp(&first_cid->id, &second_cid->id) == 0);
	CHECK_TRUE(test, first_cseq->cseq == second_cseq->cseq);
	CHECK_TRUE(test, pjsip_method_cmp(&first_cseq->method,
					 &second_cseq->method) == 0);
	CHECK_TRUE(test, (first->body == NULL) == (second->body == NULL));
	if (first->body) {
		CHECK_TRUE(test, first->body->len == second->body->len);
		CHECK_TRUE(test, pj_memcmp(first->body->data, second->body->data,
					    first->body->len) == 0);
	}
	return 0;
}

static int parse_print_reparse(pj_pool_t *pool, const char *wire,
			       pjsip_msg_type_e expected_type, int expected_code)
{
	const char *test = "message parse/print/reparse";
	pjsip_parser_err_report errors;
	pjsip_parser_err_report reparse_errors;
	pjsip_msg *message;
	pjsip_msg *reparsed;
	char *input = pool_copy_text(pool, wire);
	char *printed = pj_pool_alloc(pool, PJSIP_MAX_PKT_LEN + 1);
	pj_ssize_t printed_length;

	pj_list_init(&errors);
	message = pjsip_parse_msg(pool, input, strlen(input), &errors);
	CHECK_TRUE(test, message != NULL && pj_list_empty(&errors));
	CHECK_TRUE(test, message->type == expected_type);
	if (expected_type == PJSIP_REQUEST_MSG) {
		pjsip_sip_uri *uri;

		CHECK_TRUE(test, message->line.req.method.id == PJSIP_OPTIONS_METHOD);
		uri = (pjsip_sip_uri *)pjsip_uri_get_uri(message->line.req.uri);
		CHECK_TRUE(test, string_equals(&uri->user, "alice smith"));
		CHECK_TRUE(test, message->body && message->body->len == 11);
	} else {
		CHECK_TRUE(test, message->line.status.code == expected_code);
	}

	printed_length = pjsip_msg_print(message, printed, PJSIP_MAX_PKT_LEN);
	CHECK_TRUE(test, printed_length > 0 && printed_length <= PJSIP_MAX_PKT_LEN);
	printed[printed_length] = '\0';
	pj_list_init(&reparse_errors);
	reparsed = pjsip_parse_msg(pool, printed, printed_length, &reparse_errors);
	CHECK_TRUE(test, reparsed != NULL && pj_list_empty(&reparse_errors));
	return compare_message_semantics(message, reparsed);
}

static int test_messages(pj_pool_t *pool)
{
	const char *test = "malformed message rejection";
	static const char request[] =
		"OPTIONS sip:alice%20smith@example.com;transport=udp SIP/2.0\r\n"
		"Via: SIP/2.0/UDP client.example.com;branch=z9hG4bK-phase5\r\n"
		"From: \"Alice Smith\" <sip:alice@example.net>;tag=from-phase5\r\n"
		"To: <sips:service@example.com>\r\n"
		"Call-ID: phase5-request@example.net\r\n"
		"CSeq: 42 OPTIONS\r\n"
		"Contact: <sip:alice@client.example.com>\r\n"
		"Max-Forwards: 70\r\n"
		"Content-Type: text/plain\r\n"
		"Content-Length: 11\r\n\r\n"
		"hello=world";
	static const char response[] =
		"SIP/2.0 486 Busy Here\r\n"
		"Via: SIP/2.0/UDP client.example.com;branch=z9hG4bK-phase5\r\n"
		"From: \"Alice Smith\" <sip:alice@example.net>;tag=from-phase5\r\n"
		"To: <sips:service@example.com>;tag=to-phase5\r\n"
		"Call-ID: phase5-request@example.net\r\n"
		"CSeq: 42 OPTIONS\r\n"
		"Content-Length: 0\r\n\r\n";
	static const char *const malformed[] = {
		"BROKEN\r\n\r\n",
		"INVITE sip:alice@example.com SIP/3.0\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 XX Busy\r\nContent-Length: 0\r\n\r\n",
	};
	static const char partial[] =
		"MESSAGE sip:bob@example.com SIP/2.0\r\nContent-Length: 5\r\n\r\nabc";
	unsigned i;
	pj_size_t message_size = 0;

	if (parse_print_reparse(pool, request, PJSIP_REQUEST_MSG, 0) != 0 ||
	    parse_print_reparse(pool, response, PJSIP_RESPONSE_MSG, 486) != 0)
		return -1;

	for (i = 0; i < PJ_ARRAY_SIZE(malformed); ++i) {
		pjsip_parser_err_report errors;
		char *input = pool_copy_text(pool, malformed[i]);
		pjsip_msg *message;

		pj_list_init(&errors);
		message = pjsip_parse_msg(pool, input, strlen(input), &errors);
		CHECK_TRUE(test, message == NULL || !pj_list_empty(&errors));
	}
	CHECK_TRUE(test, pjsip_find_msg(partial, strlen(partial), PJ_FALSE,
					 &message_size) == PJSIP_EPARTIALMSG);
	CHECK_TRUE(test, message_size > strlen(partial));

	printk("[Phase 5] request/response parse, semantic round-trip, rejection: PASSED\n");
	return 0;
}

static pj_bool_t body_equals(const pjsip_msg_body *body, const char *expected)
{
	pj_size_t length = strlen(expected);

	return body && body->len == length &&
	       pj_memcmp(body->data, expected, length) == 0;
}

static int validate_multipart(const pjsip_msg_body *body)
{
	const char *test = "multipart validation";
	pjsip_multipart_part *first;
	pjsip_multipart_part *second;

	first = pjsip_multipart_get_first_part(body);
	CHECK_TRUE(test, first && body_equals(first->body, "alpha"));
	second = pjsip_multipart_get_next_part(body, first);
	CHECK_TRUE(test, second && body_equals(second->body, "{\"a\":1}"));
	CHECK_TRUE(test, pjsip_multipart_get_next_part(body, second) == NULL);
	return 0;
}

static int test_multipart(pj_pool_t *pool)
{
	const char *test = "multipart parse/print";
	static const char wire[] =
		"Multipart prologue\r\n"
		"--phase5-boundary\r\n"
		"Content-Type: text/plain\r\n\r\n"
		"alpha\r\n"
		"--phase5-boundary\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: 7\r\n\r\n"
		"{\"a\":1}\r\n"
		"--phase5-boundary--\r\n"
		"Multipart epilogue";
	pjsip_media_type content_type;
	pjsip_param boundary;
	pjsip_msg_body *body;
	pjsip_msg_body *reparsed;
	pj_str_t raw_boundary;
	pj_str_t raw_data;
	char *input = pool_copy_text(pool, wire);
	char *printed = pj_pool_alloc(pool, PJSIP_MAX_PKT_LEN + 1);
	int printed_length;

	pjsip_media_type_init2(&content_type, "multipart", "mixed");
	pj_list_init(&boundary);
	boundary.name = pj_str("boundary");
	boundary.value = pj_str("phase5-boundary");
	pj_list_push_back(&content_type.param, &boundary);

	body = pjsip_multipart_parse(pool, input, strlen(input), &content_type, 0);
	CHECK_TRUE(test, body != NULL);
	if (validate_multipart(body) != 0)
		return -1;
	CHECK_STATUS(test, pjsip_multipart_get_raw(body, &raw_boundary, &raw_data));
	CHECK_TRUE(test, string_equals(&raw_boundary, "phase5-boundary"));
	CHECK_TRUE(test, raw_data.slen == (pj_ssize_t)strlen(wire));

	printed_length = body->print_body(body, printed, PJSIP_MAX_PKT_LEN);
	CHECK_TRUE(test, printed_length > 0 && printed_length <= PJSIP_MAX_PKT_LEN);
	printed[printed_length] = '\0';
	reparsed = pjsip_multipart_parse(pool, printed, printed_length,
					 &body->content_type, 0);
	CHECK_TRUE(test, reparsed != NULL);
	if (validate_multipart(reparsed) != 0)
		return -1;

	printk("[Phase 5] multipart parse/print/reparse: PASSED\n");
	return 0;
}

static unsigned module_load_count;
static unsigned module_start_count;
static unsigned module_stop_count;
static unsigned module_unload_count;
static unsigned endpoint_exit_count;
static pjsip_endpoint *expected_module_endpoint;

static pj_status_t phase5_module_load(pjsip_endpoint *endpt)
{
	if (endpt != expected_module_endpoint)
		return PJ_EINVAL;
	++module_load_count;
	return PJ_SUCCESS;
}

static pj_status_t phase5_module_start(void)
{
	++module_start_count;
	return PJ_SUCCESS;
}

static pj_status_t phase5_module_stop(void)
{
	++module_stop_count;
	return PJ_SUCCESS;
}

static pj_status_t phase5_module_unload(void)
{
	++module_unload_count;
	return PJ_SUCCESS;
}

static void phase5_endpoint_exit(pjsip_endpoint *endpt)
{
	PJ_UNUSED_ARG(endpt);
	++endpoint_exit_count;
}

static int test_module_registration(pjsip_endpoint *endpt)
{
	const char *test = "module registration";
	pjsip_module module;
	pj_status_t status;

	module_load_count = 0;
	module_start_count = 0;
	module_stop_count = 0;
	module_unload_count = 0;
	expected_module_endpoint = endpt;
	pj_bzero(&module, sizeof(module));
	pj_list_init(&module);
	module.name = pj_str("phase5-validation");
	module.id = -1;
	module.priority = PJSIP_MOD_PRIORITY_APPLICATION;
	module.load = phase5_module_load;
	module.start = phase5_module_start;
	module.stop = phase5_module_stop;
	module.unload = phase5_module_unload;

	status = pjsip_endpt_register_module(endpt, &module);
	if (status != PJ_SUCCESS)
		return fail_status(test, __LINE__, status);
	if (module.id < 0 || module_load_count != 1 || module_start_count != 1) {
		fail_value(test, __LINE__,
			   "module loaded and started exactly once");
		pjsip_endpt_unregister_module(endpt, &module);
		expected_module_endpoint = NULL;
		return -1;
	}
	status = pjsip_endpt_unregister_module(endpt, &module);
	if (status != PJ_SUCCESS) {
		expected_module_endpoint = NULL;
		return fail_status(test, __LINE__, status);
	}
	CHECK_TRUE(test, module.id == -1 && module_stop_count == 1 &&
			 module_unload_count == 1);
	expected_module_endpoint = NULL;
	printk("[Phase 5] module load/start/stop/unload: PASSED\n");
	return 0;
}

static int test_error_strings(void)
{
	const char *test = "PJSIP error registration";
	char buffer[PJ_ERR_MSG_SIZE];
	pj_str_t result;

	result = pj_strerror(PJSIP_EINVALIDURI, buffer, sizeof(buffer));
	CHECK_TRUE(test, string_starts_with(&result, "Invalid URI"));
	result = pj_strerror(PJSIP_ERRNO_FROM_SIP_STATUS(486), buffer,
			    sizeof(buffer));
	CHECK_TRUE(test, string_starts_with(&result, "Busy Here"));
	printk("[Phase 5] PJSIP error/status strings: PASSED\n");
	return 0;
}

static int test_digest(void)
{
	const char *test = "Digest authentication";
	char response_buffer[PJSIP_MD5STRLEN + 1] = {0};
	pj_str_t response = {response_buffer, PJSIP_MD5STRLEN};
	pj_str_t nonce = pj_str("dcd98b7102dd2f0e8b11d0f600bfb0c093");
	pj_str_t nc = pj_str("00000001");
	pj_str_t cnonce = pj_str("0a4f113b");
	pj_str_t qop = pj_str("auth");
	pj_str_t uri = pj_str("/dir/index.html");
	pj_str_t realm = pj_str("testrealm@host.com");
	pj_str_t method = pj_str("GET");
	pjsip_cred_info credential;

	pj_bzero(&credential, sizeof(credential));
	credential.realm = realm;
	credential.scheme = pj_str("Digest");
	credential.username = pj_str("Mufasa");
	credential.data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
	credential.data = pj_str("Circle Of Life");
	CHECK_STATUS(test, pjsip_auth_create_digest2(&response, &nonce, &nc,
						    &cnonce, &qop, &uri,
						    &realm, &credential, &method,
						    PJSIP_AUTH_ALGORITHM_MD5));
	response_buffer[response.slen] = '\0';
	CHECK_TRUE(test, string_equals(&response,
				       "6629fae49393a05397450978507c4ef1"));
	printk("[Phase 5] RFC Digest MD5 vector: PASSED\n");
	return 0;
}

static pj_bool_t has_mandatory_headers(const pjsip_msg *message)
{
	return pjsip_msg_find_hdr(message, PJSIP_H_VIA, NULL) != NULL &&
	       pjsip_msg_find_hdr(message, PJSIP_H_FROM, NULL) != NULL &&
	       pjsip_msg_find_hdr(message, PJSIP_H_TO, NULL) != NULL &&
	       pjsip_msg_find_hdr(message, PJSIP_H_CALL_ID, NULL) != NULL &&
	       pjsip_msg_find_hdr(message, PJSIP_H_CSEQ, NULL) != NULL;
}

static int release_tdata(const char *test, pjsip_tx_data **tdata)
{
	pj_status_t status;

	if (*tdata == NULL)
		return 0;
	status = pjsip_tx_data_dec_ref(*tdata);
	*tdata = NULL;
	if (status != PJSIP_EBUFDESTROYED)
		return fail_status(test, __LINE__, status);
	return 0;
}

static int test_transmit_data(pjsip_endpoint *endpt)
{
	const char *test = "request/response/ACK/CANCEL transmit data";
	pj_str_t target = pj_str("sip:bob@example.com");
	pj_str_t from = pj_str("\"Alice\" <sip:alice@example.net>");
	pj_str_t to = pj_str("Bob <sip:bob@example.com>");
	pj_str_t contact = pj_str("<sip:alice@client.example.net>");
	pj_str_t call_id = pj_str("phase5-txdata@example.net");
	pj_str_t body = pj_str("hello");
	pjsip_tx_data *invite = NULL;
	pjsip_tx_data *response = NULL;
	pjsip_tx_data *cancel = NULL;
	pjsip_tx_data *ack = NULL;
	pjsip_rx_data request_rdata;
	pjsip_rx_data response_rdata;
	pj_status_t status;
	int result = -1;

	status = pjsip_endpt_create_request(endpt, &pjsip_invite_method, &target,
					    &from, &to, &contact, &call_id,
					    42, &body, &invite);
	if (status != PJ_SUCCESS) {
		fail_status(test, __LINE__, status);
		goto cleanup;
	}
	if (invite->msg->type != PJSIP_REQUEST_MSG ||
	    invite->msg->line.req.method.id != PJSIP_INVITE_METHOD ||
	    !has_mandatory_headers(invite->msg) || invite->msg->body == NULL) {
		fail_value(test, __LINE__, "valid INVITE transmit data");
		goto cleanup;
	}

	pj_bzero(&request_rdata, sizeof(request_rdata));
	request_rdata.msg_info.msg = invite->msg;
	request_rdata.msg_info.via = (pjsip_via_hdr *)pjsip_msg_find_hdr(
		invite->msg, PJSIP_H_VIA, NULL);
	request_rdata.msg_info.from = (pjsip_from_hdr *)pjsip_msg_find_hdr(
		invite->msg, PJSIP_H_FROM, NULL);
	request_rdata.msg_info.to = (pjsip_to_hdr *)pjsip_msg_find_hdr(
		invite->msg, PJSIP_H_TO, NULL);
	request_rdata.msg_info.cid = (pjsip_cid_hdr *)pjsip_msg_find_hdr(
		invite->msg, PJSIP_H_CALL_ID, NULL);
	request_rdata.msg_info.cseq = (pjsip_cseq_hdr *)pjsip_msg_find_hdr(
		invite->msg, PJSIP_H_CSEQ, NULL);

	status = pjsip_endpt_create_response(endpt, &request_rdata, 486, NULL,
					     &response);
	if (status != PJ_SUCCESS) {
		fail_status(test, __LINE__, status);
		goto cleanup;
	}
	if (response->msg->type != PJSIP_RESPONSE_MSG ||
	    response->msg->line.status.code != 486 ||
	    !has_mandatory_headers(response->msg)) {
		fail_value(test, __LINE__, "valid response transmit data");
		goto cleanup;
	}

	status = pjsip_endpt_create_cancel(endpt, invite, &cancel);
	if (status != PJ_SUCCESS) {
		fail_status(test, __LINE__, status);
		goto cleanup;
	}
	if (cancel->msg->type != PJSIP_REQUEST_MSG ||
	    cancel->msg->line.req.method.id != PJSIP_CANCEL_METHOD ||
	    !has_mandatory_headers(cancel->msg)) {
		fail_value(test, __LINE__, "valid CANCEL transmit data");
		goto cleanup;
	}

	pj_bzero(&response_rdata, sizeof(response_rdata));
	response_rdata.msg_info.msg = response->msg;
	response_rdata.msg_info.to = (pjsip_to_hdr *)pjsip_msg_find_hdr(
		response->msg, PJSIP_H_TO, NULL);
	status = pjsip_endpt_create_ack(endpt, invite, &response_rdata, &ack);
	if (status != PJ_SUCCESS) {
		fail_status(test, __LINE__, status);
		goto cleanup;
	}
	if (ack->msg->type != PJSIP_REQUEST_MSG ||
	    ack->msg->line.req.method.id != PJSIP_ACK_METHOD ||
	    !has_mandatory_headers(ack->msg) || ack->msg->body != NULL) {
		fail_value(test, __LINE__, "valid ACK transmit data");
		goto cleanup;
	}

	result = 0;

cleanup:
	if (release_tdata(test, &ack) != 0)
		result = -1;
	if (release_tdata(test, &cancel) != 0)
		result = -1;
	if (release_tdata(test, &response) != 0)
		result = -1;
	if (release_tdata(test, &invite) != 0)
		result = -1;
	if (result == 0)
		printk("[Phase 5] request/response/ACK/CANCEL transmit data: PASSED\n");
	return result;
}

static int run_lifecycle(int iteration)
{
	pj_caching_pool caching_pool;
	pjsip_endpoint *endpoint = NULL;
	pj_pool_t *test_pool = NULL;
	pj_status_t status;
	int result = -1;

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
	status = pjsip_endpt_create(&caching_pool.factory, "phase5", &endpoint);
	if (status != PJ_SUCCESS) {
		fail_status("pjsip_endpt_create", __LINE__, status);
		goto destroy_factory;
	}
	endpoint_exit_count = 0;
	if (!string_equals(pjsip_endpt_name(endpoint), "phase5") ||
	    pjsip_endpt_get_timer_heap(endpoint) == NULL ||
	    pjsip_endpt_get_ioqueue(endpoint) == NULL ||
	    pjsip_endpt_get_tpmgr(endpoint) == NULL ||
	    pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)) != 0) {
		fail_value("endpoint resources", __LINE__, "endpoint resources initialized");
		goto destroy_endpoint;
	}
	status = pjsip_endpt_atexit(endpoint, phase5_endpoint_exit);
	if (status != PJ_SUCCESS) {
		fail_status("endpoint exit callback", __LINE__, status);
		goto destroy_endpoint;
	}

	test_pool = pjsip_endpt_create_pool(endpoint, "phase5-test", 32768, 32768);
	if (test_pool == NULL) {
		fail_value("test pool", __LINE__, "test_pool != NULL");
		goto destroy_endpoint;
	}

	if (test_error_strings() != 0 ||
	    test_module_registration(endpoint) != 0 ||
	    test_uri_parser(test_pool) != 0 ||
	    test_messages(test_pool) != 0 ||
	    test_multipart(test_pool) != 0 ||
	    test_digest() != 0 ||
	    test_transmit_data(endpoint) != 0)
		goto release_test_pool;
	if (pj_timer_heap_count(pjsip_endpt_get_timer_heap(endpoint)) != 0) {
		fail_value("endpoint timers", __LINE__, "timer heap empty before teardown");
		goto release_test_pool;
	}
	result = 0;

release_test_pool:
	pj_pool_release(test_pool);
	test_pool = NULL;
destroy_endpoint:
	pjsip_endpt_destroy(endpoint);
	endpoint = NULL;
	if (endpoint_exit_count != 1) {
		fail_value("endpoint exit callback", __LINE__, "endpoint_exit_count == 1");
		result = -1;
	}
	if (caching_pool.used_count != 0 || caching_pool.capacity != 0) {
		fail_value("endpoint pool cleanup", __LINE__,
			   "used_count == 0 && capacity == 0");
		result = -1;
	}
	if (result == 0)
		printk("[Phase 5] lifecycle %d endpoint teardown: PASSED\n", iteration);
destroy_factory:
	pj_caching_pool_destroy(&caching_pool);
shutdown:
	pj_shutdown();
	printk("[Phase 5] lifecycle %d shutdown complete\n", iteration);
	return result;
}

int phase5_core_run(void)
{
	int iteration;

	printk("[Phase 5] parser/message/endpoint validation (%d lifecycles)\n",
	       PHASE5_LIFECYCLES);
	for (iteration = 1; iteration <= PHASE5_LIFECYCLES; ++iteration) {
		if (run_lifecycle(iteration) != 0) {
			printk("PHASE 5 RESULT: FAILED at lifecycle %d\n", iteration);
			return 1;
		}
	}

	printk("PHASE 5 RESULT: PASSED (%d/%d lifecycles)\n",
	       PHASE5_LIFECYCLES, PHASE5_LIFECYCLES);
	return 0;
}
