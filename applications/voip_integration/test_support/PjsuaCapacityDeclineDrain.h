#ifndef PJSUA_CAPACITY_DECLINE_DRAIN_H
#define PJSUA_CAPACITY_DECLINE_DRAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PJSUA_CAPACITY_DECLINE_BUFFER_BYTES 4096U
#define PJSUA_CAPACITY_TO_HEADER_BYTES 256U

enum pjsua_capacity_decline_result {
	PJSUA_CAPACITY_DECLINE_ERROR = -1,
	PJSUA_CAPACITY_DECLINE_WAITING = 0,
	PJSUA_CAPACITY_DECLINE_COMPLETE = 1,
	PJSUA_CAPACITY_DECLINE_EXPIRED = 2,
};

struct pjsua_capacity_decline_drain {
	char buffer[PJSUA_CAPACITY_DECLINE_BUFFER_BYTES];
	size_t buffered;
	uint64_t started_ms;
	uint64_t last_progress_ms;
	uint32_t transaction_timeout_ms;
	uint32_t no_progress_timeout_ms;
	uint32_t acknowledged_mask;
	unsigned expected;
	unsigned acknowledged;
};

typedef int (*pjsua_capacity_ack_fn)(void *context, unsigned cseq,
				     const char *to_header);

void pjsua_capacity_decline_drain_init(
	struct pjsua_capacity_decline_drain *drain, unsigned expected,
	uint64_t now_ms, uint32_t transaction_timeout_ms,
	uint32_t no_progress_timeout_ms);

enum pjsua_capacity_decline_result pjsua_capacity_decline_drain_poll(
	const struct pjsua_capacity_decline_drain *drain, uint64_t now_ms);

enum pjsua_capacity_decline_result pjsua_capacity_decline_drain_feed(
	struct pjsua_capacity_decline_drain *drain, const char *data,
	size_t length, uint64_t now_ms, pjsua_capacity_ack_fn acknowledge,
	void *context);

#ifdef __cplusplus
}
#endif

#endif
