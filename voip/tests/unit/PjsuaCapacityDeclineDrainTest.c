#include "PjsuaCapacityDeclineDrain.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RESPONSE_COUNT 7U

struct acknowledgements {
	unsigned cseq[RESPONSE_COUNT];
	unsigned count;
};

static int failures;

#define CHECK(condition, message) do { \
	if (!(condition)) { \
		fprintf(stderr, "PjsuaCapacityDeclineDrainTest FAILED: %s\n", \
			message); \
		++failures; \
	} \
} while (0)

static int record_ack(void *opaque, unsigned cseq, const char *to_header)
{
	struct acknowledgements *acks = opaque;

	if (acks->count == RESPONSE_COUNT || strstr(to_header, ";tag=server-") == NULL)
		return -1;
	acks->cseq[acks->count++] = cseq;
	return 0;
}

static size_t make_response(char *buffer, size_t capacity, unsigned cseq)
{
	const int length = snprintf(
		buffer, capacity,
		"SIP/2.0 603 Decline\r\n"
		"Via: SIP/2.0/TCP 127.0.0.1:9;branch=fixture-%u\r\n"
		"From: <sip:peer%u@127.0.0.1>;tag=fixture-%u\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-%u\r\n"
		"Call-ID: fixture-%u@127.0.0.1\r\n"
		"CSeq: %u INVITE\r\n"
		"Content-Length: 0\r\n\r\n",
		cseq, cseq, cseq, cseq, cseq, cseq);

	return length > 0 && (size_t)length < capacity ? (size_t)length : 0U;
}

static void test_fragmented_batched_responses_use_actual_cseq_and_progress(void)
{
	static const unsigned order[RESPONSE_COUNT] = { 7U, 2U, 5U, 1U,
							  6U, 3U, 4U };
	struct pjsua_capacity_decline_drain drain;
	struct acknowledgements acks = { { 0U }, 0U };
	char responses[RESPONSE_COUNT][320];
	char batch[960];
	size_t lengths[RESPONSE_COUNT];
	size_t batch_length = 0U;
	enum pjsua_capacity_decline_result result;
	unsigned index;

	for (index = 0U; index < RESPONSE_COUNT; ++index) {
		lengths[index] = make_response(responses[index],
						 sizeof(responses[index]), order[index]);
		CHECK(lengths[index] != 0U, "fixture response fits");
	}

	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 20000U,
					  10000U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, responses[0], 17U, 100U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "partial response waits for remaining bytes");
	result = pjsua_capacity_decline_drain_feed(
		&drain, responses[0] + 17U, lengths[0] - 17U, 100U,
		record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "first complete response renews progress deadline");

	/* No bytes arrive for 9.95 seconds after the first response progressed. */
	result = pjsua_capacity_decline_drain_poll(&drain, 10050U);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "no-data interval is measured from last response progress");

	for (index = 1U; index <= 3U; ++index) {
		memcpy(batch + batch_length, responses[index], lengths[index]);
		batch_length += lengths[index];
	}
	result = pjsua_capacity_decline_drain_feed(
		&drain, batch, batch_length, 10060U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "batched responses are drained together");

	for (index = 4U; index < RESPONSE_COUNT; ++index) {
		const size_t split = lengths[index] - 11U;
		const uint64_t first_chunk_ms = UINT64_C(10100) + index * 2U;

		result = pjsua_capacity_decline_drain_feed(
			&drain, responses[index], split, first_chunk_ms,
			record_ack, &acks);
		CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
		      "fragmented tail remains buffered");
		result = pjsua_capacity_decline_drain_feed(
			&drain, responses[index] + split, lengths[index] - split,
			first_chunk_ms + 1U, record_ack, &acks);
	}

	CHECK(result == PJSUA_CAPACITY_DECLINE_COMPLETE,
	      "all seven responses complete the drain");
	CHECK(acks.count == RESPONSE_COUNT, "all seven responses are acknowledged");
	for (index = 0U; index < RESPONSE_COUNT; ++index)
		CHECK(acks.cseq[index] == order[index],
		      "ACK uses the parsed response CSeq");
}

static void test_absolute_ceiling_and_overflow_safe_boundaries(void)
{
	static const char response[] =
		"SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-1\r\n"
		"CSeq: 1 INVITE\r\n"
		"Content-Length: 0\r\n\r\n";
	struct pjsua_capacity_decline_drain drain;
	struct acknowledgements acks = { { 0U }, 0U };
	enum pjsua_capacity_decline_result result;

	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 100U, 20U,
					  20U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, response, 1U, 110U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "partial byte before transaction ceiling is accepted");
	result = pjsua_capacity_decline_drain_poll(&drain, 120U);
	CHECK(result == PJSUA_CAPACITY_DECLINE_EXPIRED,
	      "partial byte cannot extend absolute transaction ceiling");

	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 100U,
					  100U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, "x", 1U, 99U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "irrelevant byte before transaction ceiling is accepted");
	result = pjsua_capacity_decline_drain_feed(
		&drain, "y", 1U, 100U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_EXPIRED,
	      "irrelevant byte drip cannot renew transaction ceiling");

	memset(&acks, 0, sizeof(acks));
	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 100U,
					  100U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, response, sizeof(response) - 1U, 100U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_EXPIRED,
	      "input at transaction ceiling cannot revive drain");
	CHECK(acks.count == 0U, "input at transaction ceiling sends no ACK");

	memset(&acks, 0, sizeof(acks));
	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 100U,
					  100U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, response, sizeof(response) - 1U, 101U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_EXPIRED,
	      "input after transaction ceiling cannot revive drain");
	CHECK(acks.count == 0U, "input after transaction ceiling sends no ACK");

	pjsua_capacity_decline_drain_init(
		&drain, RESPONSE_COUNT, UINT64_MAX - UINT64_C(10), 20U, 20U);
	result = pjsua_capacity_decline_drain_poll(
		&drain, UINT64_MAX - UINT64_C(10));
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "wrapped clock starts before timeout");
	result = pjsua_capacity_decline_drain_poll(&drain, UINT64_C(8));
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "elapsed time just below wrapped boundary waits");
	result = pjsua_capacity_decline_drain_poll(&drain, UINT64_C(9));
	CHECK(result == PJSUA_CAPACITY_DECLINE_EXPIRED,
	      "elapsed time at wrapped boundary expires");
}

static void expect_rejected_without_ack(const char *message, const char *reason)
{
	struct pjsua_capacity_decline_drain drain;
	struct acknowledgements acks = { { 0U }, 0U };
	enum pjsua_capacity_decline_result result;

	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 1000U,
					  1000U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, message, strlen(message), 1U, record_ack, &acks);
	CHECK(result != PJSUA_CAPACITY_DECLINE_COMPLETE, reason);
	CHECK(acks.count == 0U, reason);
}

static void test_exact_bounded_response_and_header_parsing(void)
{
	static const char spoofed_status[] =
		"SIP/2.0 200 OK\r\n"
		"X-Reason: SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-1\r\n"
		"CSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char prefixed_status[] =
		"garbage SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-1\r\n"
		"CSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char spoofed_to[] =
		"SIP/2.0 603 Decline\r\n"
		"X-Note: forged To: <sip:capacity@127.0.0.1>;tag=server-1\r\n"
		"CSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char malformed_cseq[] =
		"SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-1\r\n"
		"CSeq: +1 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char zero_cseq[] =
		"SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-0\r\n"
		"CSeq: 0 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char out_of_range_cseq[] =
		"SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-8\r\n"
		"CSeq: 8 INVITE\r\nContent-Length: 0\r\n\r\n";
	static const char overflowing_cseq[] =
		"SIP/2.0 603 Decline\r\n"
		"To: <sip:capacity@127.0.0.1>;tag=server-overflow\r\n"
		"CSeq: 4294967297 INVITE\r\nContent-Length: 0\r\n\r\n";

	expect_rejected_without_ack(spoofed_status,
				    "status substring cannot spoof 603 start line");
	expect_rejected_without_ack(prefixed_status,
				    "603 status must start the response exactly");
	expect_rejected_without_ack(spoofed_to,
				    "To substring cannot spoof anchored header");
	expect_rejected_without_ack(malformed_cseq,
				    "malformed CSeq is rejected");
	expect_rejected_without_ack(zero_cseq, "zero CSeq is rejected");
	expect_rejected_without_ack(out_of_range_cseq,
				    "out-of-range CSeq is rejected");
	expect_rejected_without_ack(overflowing_cseq,
				    "overflowing decimal CSeq is rejected");
}

static void test_duplicate_cseq_does_not_ack_or_hide_unique_response(void)
{
	static const char *const responses[RESPONSE_COUNT] = {
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-1\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-2\r\nCSeq: 2 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-3\r\nCSeq: 3 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-4\r\nCSeq: 4 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-5\r\nCSeq: 5 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-6\r\nCSeq: 6 INVITE\r\nContent-Length: 0\r\n\r\n",
		"SIP/2.0 603 Decline\r\nTo: <sip:capacity@127.0.0.1>;tag=server-7\r\nCSeq: 7 INVITE\r\nContent-Length: 0\r\n\r\n",
	};
	struct pjsua_capacity_decline_drain drain;
	struct acknowledgements acks = { { 0U }, 0U };
	enum pjsua_capacity_decline_result result;
	unsigned index;

	pjsua_capacity_decline_drain_init(&drain, RESPONSE_COUNT, 0U, 1000U,
					  1000U);
	result = pjsua_capacity_decline_drain_feed(
		&drain, responses[0], strlen(responses[0]), 1U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "first unique response remains incomplete");
	result = pjsua_capacity_decline_drain_feed(
		&drain, responses[0], strlen(responses[0]), 2U, record_ack, &acks);
	CHECK(result == PJSUA_CAPACITY_DECLINE_WAITING,
	      "duplicate response remains incomplete");
	CHECK(acks.count == 1U, "duplicate CSeq invokes no second ACK");

	for (index = 1U; index < RESPONSE_COUNT; ++index) {
		result = pjsua_capacity_decline_drain_feed(
			&drain, responses[index], strlen(responses[index]),
			3U + index, record_ack, &acks);
	}
	CHECK(result == PJSUA_CAPACITY_DECLINE_COMPLETE,
	      "later unique response completes drain after duplicate");
	CHECK(acks.count == RESPONSE_COUNT,
	      "duplicate does not consume fixed ACK storage");
	for (index = 0U; index < RESPONSE_COUNT; ++index)
		CHECK(acks.cseq[index] == index + 1U,
		      "every unique CSeq has one ACK obligation");
}

int main(void)
{
	test_fragmented_batched_responses_use_actual_cseq_and_progress();
	test_absolute_ceiling_and_overflow_safe_boundaries();
	test_exact_bounded_response_and_header_parsing();
	test_duplicate_cseq_does_not_ack_or_hide_unique_response();
	if (failures != 0)
		return 1;
	puts("PjsuaCapacityDeclineDrainTest PASSED");
	return 0;
}
