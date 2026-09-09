#include "PjsuaCapacityDeclineDrain.h"

#include <limits.h>
#include <string.h>

static void consume(struct pjsua_capacity_decline_drain *drain, char *end)
{
	const size_t consumed = (size_t)(end - drain->buffer) + 4U;

	memmove(drain->buffer, drain->buffer + consumed,
		drain->buffered - consumed);
	drain->buffered -= consumed;
	drain->buffer[drain->buffered] = '\0';
}

static const char *find_crlf(const char *cursor, const char *limit)
{
	while (cursor + 1 < limit) {
		if (cursor[0] == '\r' && cursor[1] == '\n')
			return cursor;
		++cursor;
	}
	return NULL;
}

static int find_header(const char *message, size_t message_length,
		       const char *name, const char **value,
		       size_t *value_length)
{
	const size_t name_length = strlen(name);
	const char *limit = message + message_length;
	const char *line_end = find_crlf(message, limit);
	const char *cursor;

	if (line_end == NULL)
		return -1;
	cursor = line_end + 2;
	while (cursor < limit) {
		line_end = find_crlf(cursor, limit);
		if (line_end == NULL)
			line_end = limit;
		if ((size_t)(line_end - cursor) > name_length + 1U &&
		    memcmp(cursor, name, name_length) == 0 &&
		    cursor[name_length] == ':' && cursor[name_length + 1U] == ' ') {
			*value = cursor + name_length + 2U;
			*value_length = (size_t)(line_end - *value);
			return 0;
		}
		if (line_end == limit)
			break;
		cursor = line_end + 2;
	}
	return -1;
}

static int parse_cseq(const char *value, size_t length, unsigned *cseq)
{
	static const char suffix[] = " INVITE";
	size_t index = 0U;
	unsigned parsed = 0U;
	unsigned digit;

	if (length == 0U || value[0] < '0' || value[0] > '9')
		return -1;
	while (index < length && value[index] >= '0' && value[index] <= '9') {
		digit = (unsigned)(value[index] - '0');
		if (parsed > (UINT_MAX - digit) / 10U)
			return -1;
		parsed = parsed * 10U + digit;
		++index;
	}
	if (length - index != sizeof(suffix) - 1U ||
	    memcmp(value + index, suffix, sizeof(suffix) - 1U) != 0)
		return -1;
	*cseq = parsed;
	return 0;
}

static int elapsed(uint64_t now_ms, uint64_t since_ms, uint32_t timeout_ms)
{
	return now_ms - since_ms >= (uint64_t)timeout_ms;
}

void pjsua_capacity_decline_drain_init(
	struct pjsua_capacity_decline_drain *drain, unsigned expected,
	uint64_t now_ms, uint32_t transaction_timeout_ms,
	uint32_t no_progress_timeout_ms)
{
	memset(drain, 0, sizeof(*drain));
	drain->started_ms = now_ms;
	drain->last_progress_ms = now_ms;
	drain->transaction_timeout_ms = transaction_timeout_ms;
	drain->no_progress_timeout_ms = no_progress_timeout_ms;
	drain->expected = expected;
}

enum pjsua_capacity_decline_result pjsua_capacity_decline_drain_poll(
	const struct pjsua_capacity_decline_drain *drain, uint64_t now_ms)
{
	if (drain->acknowledged == drain->expected)
		return PJSUA_CAPACITY_DECLINE_COMPLETE;
	return elapsed(now_ms, drain->started_ms,
		       drain->transaction_timeout_ms) ||
	       elapsed(now_ms, drain->last_progress_ms,
		       drain->no_progress_timeout_ms) ?
		PJSUA_CAPACITY_DECLINE_EXPIRED : PJSUA_CAPACITY_DECLINE_WAITING;
}

enum pjsua_capacity_decline_result pjsua_capacity_decline_drain_feed(
	struct pjsua_capacity_decline_drain *drain, const char *data,
	size_t length, uint64_t now_ms, pjsua_capacity_ack_fn acknowledge,
	void *context)
{
	enum pjsua_capacity_decline_result result;

	if (drain->expected == 0U || drain->expected > 32U)
		return PJSUA_CAPACITY_DECLINE_ERROR;
	result = pjsua_capacity_decline_drain_poll(drain, now_ms);
	if (result != PJSUA_CAPACITY_DECLINE_WAITING)
		return result;
	if (length > sizeof(drain->buffer) - 1U - drain->buffered)
		return PJSUA_CAPACITY_DECLINE_ERROR;
	memcpy(drain->buffer + drain->buffered, data, length);
	drain->buffered += length;
	drain->buffer[drain->buffered] = '\0';
	if (length != 0U)
		drain->last_progress_ms = now_ms;

	while (drain->acknowledged < drain->expected) {
		char *end = strstr(drain->buffer, "\r\n\r\n");
		char to_header[PJSUA_CAPACITY_TO_HEADER_BYTES];
		const char *to_value;
		const char *cseq_value;
		size_t message_length;
		size_t to_length;
		size_t cseq_length;
		unsigned cseq;
		uint32_t cseq_bit;

		if (end == NULL)
			break;
		message_length = (size_t)(end - drain->buffer);
		if (message_length < sizeof("SIP/2.0 603 Decline\r\n") - 1U ||
		    memcmp(drain->buffer, "SIP/2.0 603 Decline\r\n",
			   sizeof("SIP/2.0 603 Decline\r\n") - 1U) != 0) {
			consume(drain, end);
			continue;
		}
		if (find_header(drain->buffer, message_length, "To", &to_value,
				&to_length) != 0 ||
		    find_header(drain->buffer, message_length, "CSeq", &cseq_value,
				&cseq_length) != 0)
			return PJSUA_CAPACITY_DECLINE_ERROR;
		if (to_length == 0U || to_length >= sizeof(to_header))
			return PJSUA_CAPACITY_DECLINE_ERROR;
		if (parse_cseq(cseq_value, cseq_length, &cseq) != 0 || cseq == 0U ||
		    cseq > drain->expected)
			return PJSUA_CAPACITY_DECLINE_ERROR;
		memcpy(to_header, to_value, to_length);
		to_header[to_length] = '\0';
		cseq_bit = UINT32_C(1) << (cseq - 1U);
		if ((drain->acknowledged_mask & cseq_bit) != 0U) {
			consume(drain, end);
			continue;
		}
		if (acknowledge(context, cseq, to_header) != 0)
			return PJSUA_CAPACITY_DECLINE_ERROR;
		drain->acknowledged_mask |= cseq_bit;
		++drain->acknowledged;
		consume(drain, end);
	}
	return pjsua_capacity_decline_drain_poll(drain, now_ms);
}
