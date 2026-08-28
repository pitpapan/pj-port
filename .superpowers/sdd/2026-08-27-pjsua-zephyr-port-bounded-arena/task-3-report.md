# Task 3 report — bounded Zephyr PJ pool arena

## Scope and outcome

Task 3 replaces PJ pool block allocation with one deterministic static Zephyr
arena. The standalone profile proves exhaustion, non-throwing OOM behavior,
boundary-tag splitting/coalescing, reset semantics, and 100 varied-size
exhaustion/recovery cycles. Task 4 remains responsible for calling the install
hook from `pjsua_link.c` before `pjsua_create()`; this task does not modify
PJSUA lifecycle code or add business state-machine behavior.

TLS, SRTP, PJSUA2, and PJ worker threads remain disabled. No malloc fallback is
present in the arena allocator.

## Files changed

- `pjproject/zephyr/include/pj_zephyr_pool_arena.h` — public stats and API.
- `pjproject/zephyr/pj_zephyr_pool_arena.c` — static arena and PJ policy
  callbacks.
- `pjproject/zephyr/CMakeLists.txt` — public include and allocator source
  wiring under `CONFIG_PJSUA`.
- `applications/voip_integration/src/pjsua_arena_test.c` — standalone runtime
  proof.
- `applications/voip_integration/Kconfig` and `CMakeLists.txt` — test option
  and source wiring.
- `applications/voip_integration/pjsua_arena.conf` — 2 MiB arena test profile
  with a 1 MiB general libc malloc arena so the 4 MiB QEMU RAM image links and
  runs; the dedicated PJ arena remains `CONFIG_PJSUA_ARENA_BYTES=2097152`.
- This report and the SDD progress ledger.

## Public contract and design invariants

The header exposes exactly the requested `size_t` capacity/usage fields,
`uint32_t` block/failure fields, and `pj_status_t` install/reset functions.

- Storage is one `alignas(max_align_t) static uint8_t` array sized by
  `CONFIG_PJSUA_ARENA_BYTES`.
- Every block has aligned header/footer boundary tags with size, previous-size,
  magic, and free-state metadata. The first-fit scan validates tags before
  trusting sizes.
- Requests are alignment-rounded with checked addition; invalid/overflowing
  requests and exhausted scans return `NULL`. There is no allocation fallback.
- Splitting creates an aligned free remainder and repairs the physically next
  block's `prev_size`. Freeing merges next then previous neighbors and repairs
  the following boundary tag.
- All metadata, counters, callback recording, and stats scans are serialized
  by `k_spinlock`. `used_bytes` includes allocator block metadata, and
  `largest_free_block` reports complete free-block sizes in the same units.
- Successful reset scrubs the entire arena and reinitializes the root block and
  counters. Reset returns `PJ_EBUSY` while any block remains live and
  `PJ_EINVAL` before installation.
- Installation replaces all three `pj_pool_factory_default_policy` callbacks:
  block allocation, block free, and the `void` failure callback. The callback
  records the failure and returns, allowing PJ's pointer-returning allocation
  path to report OOM as `NULL` without aborting.

## TDD evidence

The standalone test/config/CMake wiring was built before the production source
was added. The RED command was run from `/home/pitpapan/zephyrproject`:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DISABLE=1 west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task3-red -- \
  -DEXTRA_CONF_FILE=pjsua_arena.conf
```

It exited 1 at final link with the expected missing symbols from
`pjsua_arena_test.c`:

```text
undefined reference to `pj_zephyr_pool_arena_install'
undefined reference to `pj_zephyr_pool_arena_get_stats'
undefined reference to `pj_zephyr_pool_arena_reset'
```

No allocator implementation source existed at the RED checkpoint.

## GREEN build and runtime evidence

After implementing the allocator, the clean sequential build was:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task3-green -- \
  -DEXTRA_CONF_FILE=pjsua_arena.conf
```

Result: exit 0. Final image footprint:

```text
FLASH:       75724 B / 4 MB (1.81%)
RAM:       3527192 B / 4 MB (84.09%)
```

The runtime command, from `/home/pitpapan/zephyrproject`, was:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
west build -d /tmp/voip-plan1-task3-green -t run
```

QEMU printed the required clear marker and then idled:

```text
PJSUA ARENA RESULT: PASSED (exhaustion, coalescing, 100 cycles)
```

The test specifically covers:

1. varied-size PJ pool allocations until deterministic exhaustion;
2. failure accounting and an expansion allocation returning `NULL` after the
   non-throwing PJ failure callback;
3. `PJ_EBUSY` reset with live allocations;
4. alternating frees that preserve fragmentation, then complete adjacent
   coalescing and full recovery;
5. an interior split followed by freeing the following allocation, proving the
   repaired `prev_size` enables backward coalescing;
6. successful scrub/reinitialize reset; and
7. 100 cycles of varied-size exhaustion, release, reset, and complete recovery.

## Risks and remaining limitations

- The arena is process-global, as is `pj_pool_factory_default_policy`; the
  lifecycle owner must install it before creating PJSUA pools and must only
  reset after all PJ pools are released.
- The standalone profile's 1 MiB general libc arena is test-profile memory
  tuning for the 4 MiB QEMU board. It does not change the dedicated PJ arena
  capacity or claim that later production profiles need no additional RAM
  measurement.
- Invalid external free pointers are ignored after tag/range validation; no
  diagnostic counter is exposed by the exact public contract.
- Task 4 still needs to prove PJSUA lifecycle cleanup and installation timing.

## Status and commit

`git diff --check` passed, and the worktree contains only the files listed
above plus the progress ledger. The Task 3 implementation commit SHA is
recorded below after commit creation.

- Commit message: `feat(pjlib): add bounded Zephyr pool arena`
- Commit SHA: `6d3ed9125`
