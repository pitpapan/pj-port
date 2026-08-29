# Task 4 report — actor-driven bounded PJSUA lifecycle

## Scope and outcome

The PJSUA link probe now installs the Task 3 bounded pool arena before every
PJSUA allocation path, resets it before each lifecycle, and verifies arena
statistics before create, after start, and after destroy. It executes five
complete create/init/start/poll/destroy cycles with no PJSUA or media worker
threads. A callback-table `on_call_state` trampoline and a timer fallback both
record callback count and reject execution from any thread other than the
captured actor (`k_current_get()`).

## Files changed

- `applications/voip_integration/src/pjsua_link.c`
- `applications/voip_integration/pjsua_link.conf`

The link profile's general libc malloc arena is 1 MiB so the 2 MiB dedicated
PJ arena and the PJSUA image fit the 4 MiB mps2/an385 QEMU RAM region. The
dedicated PJ arena remains exactly `CONFIG_PJSUA_ARENA_BYTES=2097152`; no
general-heap fallback was added.

## TDD evidence

Assertions and callback behavior were added first, while arena installation
was intentionally absent. The first build attempt exposed an expected profile
RAM overflow (the prior 3 MiB libc arena plus the new 2 MiB static arena). The
profile was reduced to the established 1 MiB test value, then the assertion-
first RED build completed and its QEMU run failed for the intended missing
Task 4 integration:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task4-red -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
```

The corrected RED build exited 0, but the bounded QEMU run exited 124 after
the harness timeout and printed:

```text
PJSUA LINK CHECK FAILED: arena peak is bounded and non-zero
PJSUA LINK CHECK FAILED: arena is clean after destroy
PJSUA LINK RESULT: FAILED
```

This is the expected failure: PJSUA was still using the uninstalled default
pool policy, so arena peak remained zero and the post-destroy arena state was
not meaningful. It was not a compile typo or link failure.

## GREEN focused acceptance

After adding `pj_zephyr_pool_arena_install()` before `pjsua_create()` and
`pj_zephyr_pool_arena_reset()` before each cycle, the focused incremental
build exited 0:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build \
  -d /tmp/voip-plan1-task4-green
```

The preceding pristine build of the same source/profile also exited 0 and
reported FLASH 526948 B (12.56%) and RAM 3734744 B (89.04%). The final focused
run was:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
timeout 30s west build -d /tmp/voip-plan1-task4-green -t run
```

It exited 124 only when the bounded QEMU harness stopped the idle emulator,
after printing the required marker:

```text
PJSUA LINK RESULT: PASSED (5 lifecycles, arena clean)
```

The run also printed one non-vacuous callback observation per cycle:

```text
PJSUA LINK lifecycle 1: callbacks=1 actor-affine
PJSUA LINK lifecycle 2: callbacks=1 actor-affine
PJSUA LINK lifecycle 3: callbacks=1 actor-affine
PJSUA LINK lifecycle 4: callbacks=1 actor-affine
PJSUA LINK lifecycle 5: callbacks=1 actor-affine
```

Each of the five cycles also logged PJSUA's `No SIP worker threads created`.
The caller loop invokes `pjsua_handle_events(10)` first for the bare lifecycle;
when no callback is observed, it schedules `pjsua_schedule_timer2(..., 0)` and
polls again. Thus callback count cannot pass vacuously, and every observed
callback passes the captured actor-thread check. The installed callback-table
trampoline routes through the same affinity guard.

## Fix round 1: review findings and TDD evidence

The review fixes are confined to `applications/voip_integration/src/pjsua_link.c`.
`poll_until_callback()` now has a compile-time-overridable limit of 100 polls,
with each poll bounded to 10 ms. Exhaustion prints the callback timeout failure,
sets `PJ_ETIMEDOUT`, and continues through the common destroy/audit path. Callback
count and affinity are `atomic_t`, so a prohibited foreign-thread callback cannot
race the actor's observations. `destroy_and_audit()` is used for normal teardown,
the configuration-error edge, and a failed `pjsua_create()`; it always collects
post-destroy arena statistics and requires zero live blocks/used bytes. The
normal path performs a second affinity check after destruction, covering late
callbacks during `pjsua_destroy()`.

The timeout behavior was tested first with a focused compile-time seam, before
the final normal build:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \\
  -b mps2/an385 \\
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \\
  -d /tmp/voip-plan1-task4-fix-timeout-red -- \\
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf \\
  -DCMAKE_C_FLAGS=-DPJSUA_EVENT_POLL_LIMIT=0
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
timeout 15s west build -d /tmp/voip-plan1-task4-fix-timeout-red -t run
```

The build exited 0. The intentionally failing probe run exited 124 only when
the bounded QEMU harness stopped the idle emulator, and printed
`PJSUA LINK CHECK FAILED: callback dispatch timeout`, followed by the complete
PJSUA destroy log and `PJSUA LINK RESULT: FAILED`. This is the expected RED
behavior for a zero poll budget: it demonstrates that timeout does not hang and
reaches cleanup. The temporary red directory was removed before the acceptance
build.

The allocation-failure cleanup edge was then exercised with the dedicated arena
temporarily set to `CONFIG_PJSUA_ARENA_BYTES=65536` in the same application
profile. The exact build/run commands were:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \\
  -b mps2/an385 \\
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \\
  -d /tmp/voip-plan1-task4-fix -- \\
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
timeout 15s west build -d /tmp/voip-plan1-task4-fix -t run
```

That build exited 0 and the intentionally undersized run reached
`pjsua_aud.c ..Error registering codecs: Not enough memory (PJ_ENOMEM)`, then
completed PJSUA shutdown/destroy and printed `PJSUA LINK RESULT: FAILED` before
the harness timeout (exit 124). This is expected RED allocation failure and
proves the common cleanup/audit path on partial initialization. The profile was
restored to `CONFIG_PJSUA_ARENA_BYTES=2097152` before the final GREEN build.

The final focused Task 4 build exited 0 (FLASH 527260 B / 12.57%, RAM 3734744 B
/ 89.04%):

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \\
  -b mps2/an385 \\
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \\
  -d /tmp/voip-plan1-task4-fix -- \\
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
timeout 30s west build -d /tmp/voip-plan1-task4-fix -t run
```

The run exited 124 only when the bounded harness stopped QEMU and printed all
required evidence:

```text
PJSUA LINK lifecycle 1: callbacks=1 actor-affine
PJSUA LINK lifecycle 2: callbacks=1 actor-affine
PJSUA LINK lifecycle 3: callbacks=1 actor-affine
PJSUA LINK lifecycle 4: callbacks=1 actor-affine
PJSUA LINK lifecycle 5: callbacks=1 actor-affine
PJSUA LINK RESULT: PASSED (5 lifecycles, arena clean)
```

Every lifecycle also logged `No SIP worker threads created`. The first event
poll is the caller loop; the timer fallback is only scheduled when that poll is
quiet, so each `callbacks=1` is non-vacuous. Both registered callback-table
trampolines use the atomic actor-affinity guard, and the post-destroy check
ensures a late callback cannot make the probe report success incorrectly.

For regression, the same fresh build directory was reconfigured with the
documented phase-3 profile and built successfully, then run with:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \\
  -b mps2/an385 \\
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \\
  -d /tmp/voip-plan1-task4-fix -- \\
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/phase3_account.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \\
timeout 20s west build -d /tmp/voip-plan1-task4-fix -t run
```

The build exited 0 (FLASH 170196 B / 4.06%, RAM 560240 B / 13.36%). QEMU
printed all three phase-3 lifecycle passes and
`VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)` before the
bounded harness timeout (exit 124).

Self-review: `git diff --check` passes; the source uses only documented PJSUA
and Zephyr atomics/event-loop APIs, preserves Task 2 compile-time/runtime
limits and the Task 1 lifecycle contract, and introduces no PJSUA2, TLS, SRTP,
worker thread, dynamic/general-heap fallback, or source glob. Existing PJ
unused-code warnings remain unchanged; no warning points at the modified probe.
The expected RAM utilization remains high but below the configured 4 MiB limit.

## Lifecycle and arena assertions

The probe captures `before` after a successful install/reset and before
`pjsua_create()`, requiring configured capacity, zero used bytes, and zero live
blocks. After `pjsua_start()`, it requires non-zero peak usage bounded by
capacity. After `pjsua_destroy()`, it requires the same capacity, zero used
bytes, zero live blocks, and a non-zero peak no larger than capacity. The
arena is reset only after the prior destroy has passed its clean-state check.
The existing Task 2 compile-time 5/7/12 and TLS/SRTP-disabled assertions and
runtime max-call/media-port checks remain intact.

## Regression evidence

A clean phase-3 account profile build exited 0:

```text
PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=4 west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task4-regression -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/phase3_account.conf
```

Its bounded QEMU run exited 124 after printing all three lifecycle results and
the required final marker:

```text
VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)
```

## Self-review and concerns

`git diff --check` passes. The implementation changes only the two files named
by the Task 4 brief. It adds no PJSUA2, TLS, SRTP, worker thread, dynamic
fallback, manifest, or source-glob behavior. Callback count is reset per
lifecycle, affinity validity is retained across the whole probe, and timer
fallback is only used after an initial caller-loop poll observes no callback.
The only concern is the expected high QEMU RAM utilization (89.04%), addressed
by the profile's 1 MiB general libc arena; the dedicated PJ arena and all
Task 2 limits remain unchanged.
