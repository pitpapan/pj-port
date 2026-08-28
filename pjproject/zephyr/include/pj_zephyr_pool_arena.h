/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef PJ_ZEPHYR_POOL_ARENA_H
#define PJ_ZEPHYR_POOL_ARENA_H

#include <pj/types.h>

#include <stddef.h>
#include <stdint.h>

PJ_BEGIN_DECL

struct pj_zephyr_pool_arena_stats {
	size_t capacity_bytes;
	size_t used_bytes;
	size_t peak_bytes;
	size_t largest_free_block;
	uint32_t live_blocks;
	uint32_t failed_allocations;
};

pj_status_t pj_zephyr_pool_arena_install(void);
pj_status_t pj_zephyr_pool_arena_reset(void);
void pj_zephyr_pool_arena_get_stats(
	struct pj_zephyr_pool_arena_stats *out);

PJ_END_DECL

#endif /* PJ_ZEPHYR_POOL_ARENA_H */
