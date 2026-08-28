#include <pjlib.h>
#include <pj_zephyr_pool_arena.h>

#include <zephyr/sys/printk.h>

#include <stddef.h>

#define TEST_POOL_COUNT 64
#define TEST_INCREMENT_BYTES 65536

static const size_t pool_sizes[] = {
	16384, 32768, 49152, 65536, 81920, 98304
};
#define POOL_SIZE_COUNT (sizeof(pool_sizes) / sizeof(pool_sizes[0]))

static int expect(int condition, const char *message)
{
	if (!condition) {
		printk("PJSUA ARENA CHECK FAILED: %s\n", message);
		return -1;
	}
	return 0;
}

static int run_arena_test(void)
{
	pj_caching_pool caching;
	pj_pool_t *pools[TEST_POOL_COUNT] = { 0 };
	struct pj_zephyr_pool_arena_stats stats;
	pj_status_t status;
	unsigned pool_count = 0;
	unsigned i;

	status = pj_init();
	if (expect(status == PJ_SUCCESS, "pj_init") != 0)
		return -1;

	status = pj_zephyr_pool_arena_install();
	if (expect(status == PJ_SUCCESS, "arena install") != 0)
		goto fail_shutdown;

	pj_zephyr_pool_arena_get_stats(&stats);
	if (expect(stats.capacity_bytes == CONFIG_PJSUA_ARENA_BYTES,
		   "arena capacity") != 0 ||
	    expect(stats.used_bytes == 0 && stats.live_blocks == 0,
		   "arena starts empty") != 0)
		goto fail_shutdown;

	pj_caching_pool_init(&caching, NULL, 0);

	/* Deliberately fill the arena with independently sized pool blocks. */
	while (pool_count < TEST_POOL_COUNT) {
		pools[pool_count] = pj_pool_create(&caching.factory, "arena",
						   pool_sizes[pool_count % POOL_SIZE_COUNT],
						   TEST_INCREMENT_BYTES, NULL);
		if (pools[pool_count] == NULL)
			break;
		pool_count++;
	}
	if (expect(pool_count > 0, "initial pool allocation") != 0 ||
	    expect(pool_count < TEST_POOL_COUNT, "deterministic exhaustion") != 0)
		goto fail_caching;

	pj_zephyr_pool_arena_get_stats(&stats);
	if (expect(stats.failed_allocations > 0, "failed allocation recorded") != 0 ||
	    expect(stats.live_blocks == pool_count, "live block accounting") != 0 ||
	    expect(stats.peak_bytes == stats.used_bytes,
		   "peak tracks initial high-water mark") != 0)
		goto fail_caching;

	status = pj_zephyr_pool_arena_reset();
	if (expect(status == PJ_EBUSY, "reset rejects live allocations") != 0)
		goto fail_caching;

	/* Free every other pool: aggregate free space grows, but remains split. */
	for (i = 0; i < pool_count; i += 2) {
		pj_pool_release(pools[i]);
		pools[i] = NULL;
	}
	pj_zephyr_pool_arena_get_stats(&stats);
	if (expect(stats.live_blocks == (pool_count / 2),
		   "alternating free accounting") != 0 ||
	    expect(stats.largest_free_block < stats.capacity_bytes - stats.used_bytes,
		   "alternating free space remains fragmented") != 0)
		goto fail_caching;

	for (i = 1; i < pool_count; i += 2) {
		pj_pool_release(pools[i]);
		pools[i] = NULL;
	}
	pj_zephyr_pool_arena_get_stats(&stats);
	if (expect(stats.used_bytes == 0 && stats.live_blocks == 0,
		   "complete recovery") != 0 ||
	    expect(stats.largest_free_block == stats.capacity_bytes,
		   "adjacent blocks coalesce") != 0)
		goto fail_caching;

	status = pj_zephyr_pool_arena_reset();
	if (expect(status == PJ_SUCCESS, "reset after release") != 0)
		goto fail_caching;

	/* A split interior block must retain a correct backward boundary tag. */
	{
		pj_pool_t *first = pj_pool_create(&caching.factory, "first",
							  pool_sizes[0], TEST_INCREMENT_BYTES, NULL);
		pj_pool_t *middle = pj_pool_create(&caching.factory, "middle",
							   pool_sizes[5], TEST_INCREMENT_BYTES, NULL);
		pj_pool_t *last = pj_pool_create(&caching.factory, "last",
							 pool_sizes[0], TEST_INCREMENT_BYTES, NULL);
		pj_pool_t *split;

		if (expect(first != NULL && middle != NULL && last != NULL,
			   "split setup allocations") != 0)
			goto fail_caching;
		pj_pool_release(middle);
		split = pj_pool_create(&caching.factory, "split", pool_sizes[0],
						       TEST_INCREMENT_BYTES, NULL);
		if (expect(split != NULL, "split allocation") != 0)
			goto fail_caching;
		pj_pool_release(last);
		pj_zephyr_pool_arena_get_stats(&stats);
		if (expect(stats.largest_free_block >= pool_sizes[5],
			   "split remainder coalesces through following tag") != 0)
			goto fail_caching;
		pj_pool_release(split);
		pj_pool_release(first);
	}
	status = pj_zephyr_pool_arena_reset();
	if (expect(status == PJ_SUCCESS, "reset after split test") != 0)
		goto fail_caching;

	/* Expansion OOM must return NULL after invoking the non-throwing callback. */
	{
		pj_pool_t *pool = pj_pool_create(&caching.factory, "oom",
							  pool_sizes[0], TEST_INCREMENT_BYTES, NULL);
		if (expect(pool != NULL, "OOM probe pool allocation") != 0)
			goto fail_caching;
		if (expect(pj_pool_alloc(pool, CONFIG_PJSUA_ARENA_BYTES) == NULL,
			   "expansion OOM returns NULL") != 0) {
			pj_pool_release(pool);
			goto fail_caching;
		}
		pj_pool_release(pool);
	}
	status = pj_zephyr_pool_arena_reset();
	if (expect(status == PJ_SUCCESS, "reset after OOM probe") != 0)
		goto fail_caching;

	for (i = 0; i < 100; ++i) {
		unsigned cycle_count = 0;
		pj_pool_t *cycle_pools[TEST_POOL_COUNT] = { 0 };

		while (cycle_count < TEST_POOL_COUNT) {
			cycle_pools[cycle_count] = pj_pool_create(
				&caching.factory, "cycle",
				pool_sizes[(cycle_count + i) % POOL_SIZE_COUNT],
				TEST_INCREMENT_BYTES, NULL);
			if (cycle_pools[cycle_count] == NULL)
				break;
			cycle_count++;
		}
		if (expect(cycle_count > 0 && cycle_count < TEST_POOL_COUNT,
			   "repeat cycle exhaustion") != 0)
			goto fail_caching;
		while (cycle_count > 0) {
			--cycle_count;
			pj_pool_release(cycle_pools[cycle_count]);
		}
		status = pj_zephyr_pool_arena_reset();
		if (expect(status == PJ_SUCCESS, "repeat cycle reset") != 0)
			goto fail_caching;
	}

	pj_zephyr_pool_arena_get_stats(&stats);
	if (expect(stats.used_bytes == 0 && stats.live_blocks == 0,
		   "100 cycles recover completely") != 0)
		goto fail_caching;

	pj_caching_pool_destroy(&caching);
	pj_shutdown();
	printk("PJSUA ARENA RESULT: PASSED (exhaustion, coalescing, 100 cycles)\n");
	return 0;

fail_caching:
	for (i = 0; i < pool_count; ++i) {
		if (pools[i] != NULL)
			pj_pool_release(pools[i]);
	}
	pj_caching_pool_destroy(&caching);
fail_shutdown:
	pj_shutdown();
	return -1;
}

int main(void)
{
	return run_arena_test();
}
