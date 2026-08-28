/* SPDX-License-Identifier: BSD-3-Clause */

#include <pj/pool.h>
#include <pj/errno.h>
#include <pj_zephyr_pool_arena.h>

#include <zephyr/kernel.h>

#include <limits.h>
#include <stdbool.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARENA_MAGIC UINT32_C(0x504A4152)
#define ARENA_ALIGNMENT alignof(max_align_t)

struct arena_header {
	size_t total_size;
	size_t prev_size;
	uint32_t magic;
	uint8_t free;
};

struct arena_footer {
	size_t total_size;
	uint32_t magic;
	uint8_t free;
	max_align_t alignment_pad;
};

_Static_assert(sizeof(struct arena_header) % ARENA_ALIGNMENT == 0,
	       "arena header must preserve alignment");
_Static_assert(sizeof(struct arena_footer) % ARENA_ALIGNMENT == 0,
	       "arena footer must preserve alignment");

/* All allocator metadata lives in this single statically reserved arena. */
alignas(max_align_t) static uint8_t arena[CONFIG_PJSUA_ARENA_BYTES];
static struct k_spinlock arena_lock;
static bool arena_installed;
static size_t arena_used;
static size_t arena_peak;
static uint32_t arena_live_blocks;
static uint32_t arena_failed_allocations;
static uint32_t arena_callback_failures;

#define ARENA_HEADER_SIZE sizeof(struct arena_header)
#define ARENA_FOOTER_SIZE sizeof(struct arena_footer)
#define ARENA_MIN_BLOCK \
	(ARENA_HEADER_SIZE + ARENA_FOOTER_SIZE + ARENA_ALIGNMENT)

static size_t align_up_size(size_t value)
{
	const size_t mask = ARENA_ALIGNMENT - 1;

	if (value > SIZE_MAX - mask)
		return 0;
	return (value + mask) & ~mask;
}

static struct arena_footer *block_footer(struct arena_header *block)
{
	return (struct arena_footer *)((uint8_t *)block +
					  block->total_size - ARENA_FOOTER_SIZE);
}

static bool valid_block(const struct arena_header *block)
{
	uintptr_t begin = (uintptr_t)arena;
	uintptr_t end = begin + sizeof(arena);
	uintptr_t address = (uintptr_t)block;
	const struct arena_footer *footer;

	if (address < begin || address >= end ||
	    block->magic != ARENA_MAGIC || block->total_size < ARENA_MIN_BLOCK ||
	    block->total_size > end - address)
		return false;

	footer = (const struct arena_footer *)((const uint8_t *)block +
						 block->total_size - ARENA_FOOTER_SIZE);
	return footer->magic == ARENA_MAGIC &&
	       footer->total_size == block->total_size &&
	       footer->free == block->free;
}

static void write_footer(struct arena_header *block)
{
	struct arena_footer *footer = block_footer(block);

	footer->total_size = block->total_size;
	footer->magic = ARENA_MAGIC;
	footer->free = block->free;
}

static void initialize_arena(void)
{
	struct arena_header *root;

	memset(arena, 0, sizeof(arena));
	root = (struct arena_header *)arena;
	root->total_size = sizeof(arena);
	root->prev_size = 0;
	root->magic = ARENA_MAGIC;
	root->free = 1;
	write_footer(root);

	arena_used = 0;
	arena_peak = 0;
	arena_live_blocks = 0;
	arena_failed_allocations = 0;
	arena_callback_failures = 0;
}

static void *arena_block_alloc(pj_pool_factory *factory, pj_size_t size)
{
	struct arena_header *block;
	size_t payload_size;
	size_t required_size;
	size_t offset = 0;
	k_spinlock_key_t key;

	PJ_UNUSED_ARG(factory);
	key = k_spin_lock(&arena_lock);

	payload_size = align_up_size((size_t)size);
	if (size == 0 || payload_size == 0 ||
	    payload_size > SIZE_MAX - ARENA_HEADER_SIZE - ARENA_FOOTER_SIZE) {
		arena_failed_allocations++;
		k_spin_unlock(&arena_lock, key);
		return NULL;
	}
	required_size = payload_size + ARENA_HEADER_SIZE + ARENA_FOOTER_SIZE;

	while (offset + ARENA_MIN_BLOCK <= sizeof(arena)) {
		block = (struct arena_header *)(arena + offset);
		if (!valid_block(block))
			break;

		if (block->free && block->total_size >= required_size) {
			size_t remainder = block->total_size - required_size;

			if (remainder >= ARENA_MIN_BLOCK) {
				struct arena_header *next =
					(struct arena_header *)((uint8_t *)block +
								       required_size);
				struct arena_header *after;

				block->total_size = required_size;
				next->total_size = remainder;
				next->prev_size = required_size;
				next->magic = ARENA_MAGIC;
				next->free = 1;
				write_footer(next);
				after = (struct arena_header *)
					((uint8_t *)next + next->total_size);
				if ((uintptr_t)after < (uintptr_t)arena + sizeof(arena) &&
				    valid_block(after))
					after->prev_size = next->total_size;
			}

			block->free = 0;
			write_footer(block);
			arena_used += block->total_size;
			if (arena_used > arena_peak)
				arena_peak = arena_used;
			arena_live_blocks++;
			k_spin_unlock(&arena_lock, key);
			return (uint8_t *)block + ARENA_HEADER_SIZE;
		}

		offset += block->total_size;
	}

	arena_failed_allocations++;
	k_spin_unlock(&arena_lock, key);
	return NULL;
}

static void arena_block_free(pj_pool_factory *factory, void *memory,
				     pj_size_t size)
{
	struct arena_header *block;
	k_spinlock_key_t key;

	PJ_UNUSED_ARG(factory);
	PJ_UNUSED_ARG(size);
	if (memory == NULL)
		return;

	key = k_spin_lock(&arena_lock);
	if ((uintptr_t)memory < (uintptr_t)arena + ARENA_HEADER_SIZE ||
	    (uintptr_t)memory >= (uintptr_t)arena + sizeof(arena))
		goto unlock;

	block = (struct arena_header *)((uint8_t *)memory - ARENA_HEADER_SIZE);
	if (!valid_block(block) || block->free)
		goto unlock;

	block->free = 1;
	if (arena_used >= block->total_size)
		arena_used -= block->total_size;
	else
		arena_used = 0;
	if (arena_live_blocks != 0)
		arena_live_blocks--;
	write_footer(block);

	/* Merge with the physically following block first. */
	{
		struct arena_header *next = (struct arena_header *)
			((uint8_t *)block + block->total_size);
		if ((uintptr_t)next < (uintptr_t)arena + sizeof(arena) &&
		    valid_block(next) && next->free) {
			block->total_size += next->total_size;
			write_footer(block);
			next = (struct arena_header *)
				((uint8_t *)block + block->total_size);
			if ((uintptr_t)next < (uintptr_t)arena + sizeof(arena) &&
			    valid_block(next))
				next->prev_size = block->total_size;
		}
	}

	/* Boundary tags make the preceding block available without a list. */
	if (block->prev_size != 0) {
		struct arena_header *previous = (struct arena_header *)
			((uint8_t *)block - block->prev_size);
		if ((uintptr_t)previous >= (uintptr_t)arena &&
		    valid_block(previous) && previous->free) {
			previous->total_size += block->total_size;
			write_footer(previous);
			block = previous;
			struct arena_header *next = (struct arena_header *)
				((uint8_t *)block + block->total_size);
			if ((uintptr_t)next < (uintptr_t)arena + sizeof(arena) &&
			    valid_block(next))
				next->prev_size = block->total_size;
		}
	}

unlock:
	k_spin_unlock(&arena_lock, key);
}

static void arena_pool_callback(pj_pool_t *pool, pj_size_t size)
{
	k_spinlock_key_t key;

	PJ_UNUSED_ARG(pool);
	PJ_UNUSED_ARG(size);
	key = k_spin_lock(&arena_lock);
	arena_callback_failures++;
	k_spin_unlock(&arena_lock, key);
}

pj_status_t pj_zephyr_pool_arena_install(void)
{
	k_spinlock_key_t key = k_spin_lock(&arena_lock);

	if (!arena_installed) {
		initialize_arena();
		arena_installed = true;
	}

	/* Install all policy callbacks before any PJSUA pool is created. */
	pj_pool_factory_default_policy.block_alloc = &arena_block_alloc;
	pj_pool_factory_default_policy.block_free = &arena_block_free;
	pj_pool_factory_default_policy.callback = &arena_pool_callback;

	k_spin_unlock(&arena_lock, key);
	return PJ_SUCCESS;
}

pj_status_t pj_zephyr_pool_arena_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&arena_lock);

	if (!arena_installed) {
		k_spin_unlock(&arena_lock, key);
		return PJ_EINVAL;
	}
	if (arena_live_blocks != 0) {
		k_spin_unlock(&arena_lock, key);
		return PJ_EBUSY;
	}

	initialize_arena();
	k_spin_unlock(&arena_lock, key);
	return PJ_SUCCESS;
}

void pj_zephyr_pool_arena_get_stats(struct pj_zephyr_pool_arena_stats *out)
{
	struct arena_header *block;
	size_t largest_free = 0;
	size_t offset = 0;
	k_spinlock_key_t key;

	if (out == NULL)
		return;

	key = k_spin_lock(&arena_lock);
	while (offset + ARENA_MIN_BLOCK <= sizeof(arena)) {
		block = (struct arena_header *)(arena + offset);
		if (!valid_block(block))
			break;
		if (block->free && block->total_size > largest_free)
			largest_free = block->total_size;
		offset += block->total_size;
	}

	out->capacity_bytes = sizeof(arena);
	out->used_bytes = arena_used;
	out->peak_bytes = arena_peak;
	out->largest_free_block = largest_free;
	out->live_blocks = arena_live_blocks;
	out->failed_allocations = arena_failed_allocations;
	k_spin_unlock(&arena_lock, key);
}
