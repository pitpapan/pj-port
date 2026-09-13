# Five-account PJSUA registration acceptance record

This record captures the reduced Plan 3 acceptance gate at revision
`c6a9a97b934790432e52db1d483287a004ebc5e7` on the QEMU `mps2/an385`
board. That revision includes the separately reviewed Plan 1 capacity-drain
harness fix, selected-PJSUA TLS/SRTP side-effect evidence, real-profile
general-heap evidence, and the clean fake-QEMU termination fix. It is evidence
for the bounded five-account PJSUA registration composition; it is not
embedded-target acceptance.

## Accepted behavior

One `VoipService`, one actor, one PJSUA runtime, one callback router, and one
plain-TCP transport manage five independently configured accounts. Four
accounts authenticate successfully and one receives the intentional 403.
Agents 1 and 3 independently recover from initial 408 responses; agents 0 and
2 independently refresh; shutdown starts while agent 1 has a refresh retry
pending. The complete behavior and cleanup boundary passed five consecutive
lifecycles.

PJSUA and PJMEDIA worker counts remain zero. `PjsuaRuntime` sets both
`pjsua_config.thread_cnt` and `pjsua_media_config.thread_cnt` to zero, and each
native lifecycle logged `No SIP worker threads created`. Only the existing
VoIP actor pumps `pjsua_handle_events()`.

## Reproduction commands

The host suite was run twice from the worktree:

```sh
cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
bash voip/tests/run_host_tests.sh 2>&1 | tee /tmp/voip-plan3-task7-final-restart-host-run1.log
bash voip/tests/run_host_tests.sh 2>&1 | tee /tmp/voip-plan3-task7-final-restart-host-run2.log
```

Both runs exited zero. Each emitted eleven exact `PASSED` markers plus the
credential-erasure evidence line, including
`AgentRegistryTest PASSED`, `CallStateMachineTest PASSED`,
`VoipEventQueueTest PASSED`, `CallSchedulerTest PASSED`,
`VoipServiceCoreTest PASSED`, `NoHeapAfterInitTest PASSED`,
`ShutdownMailboxRegressionTest PASSED`, and
`PjsuaCapacityDeclineDrainTest PASSED`.

The fresh fake Plan 3 profile was built and run from the west workspace:

```sh
cd /home/pitpapan/zephyrproject
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -p always -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-component-task7-final-restart -- \
  -DEXTRA_CONF_FILE=pjsua_registration_fake.conf \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-component-build.log

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout 18s west build -d /tmp/voip-plan3-component-task7-final-restart -t run \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-component-run.log
```

The final termination-fix rebuild and regression run were:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DISABLE=1 \
west build -d /tmp/voip-plan3-task7-final-wave-red
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
bash /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/voip/tests/pjsua/run_plan3_component_qemu_regression.sh \
  /tmp/voip-plan3-task7-final-wave-red \
  /tmp/voip-plan3-task7-final-wave-green2-run.log
```

It emitted:

```text
PjsuaRuntimeAdapterTest outstanding REGISTER retains shutdown barrier: yes
PJSUA PLAN 3 COMPONENT RESULT: PASSED
```

The regression harness requires its deliberate 18-second QEMU timeout and
requires the exact PASS marker before it. After that marker the log contains
only harness timeout cleanup: it contains no Usage Fault, `Illegal load of
EXC_RETURN into PC`, or `abort()`. The selected test main remains quiescent in
`k_sleep(K_FOREVER)` after all test-owned fake lifetimes have detached, so the
harness—not the application—owns QEMU termination.

The fresh real Plan 3 profile was built and run with:

```sh
cd /home/pitpapan/zephyrproject
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -p always -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-registration-task7-final-restart -- \
  -DEXTRA_CONF_FILE=pjsua_registration.conf \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-registration-build.log

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout 55s west build -d /tmp/voip-plan3-registration-task7-final-restart -t run \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-registration-run.log
```

It emitted one smoke marker, exactly five numbered lifecycle markers, and one
final terminator:

```text
VOIP PJSUA REGISTRATION SMOKE: PASSED (1 account, TCP Digest, actor polling)
VOIP PJSUA LIFECYCLE 1: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 2: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 3: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 4: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 5: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA GENERAL HEAP: PASSED (0 allocations after initialization)
VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)
```

There were six `No SIP worker threads created` lines: one for the smoke
lifecycle and one for each complete lifecycle. The real log contained no
application failure, Usage Fault, assertion failure, abort, or
`PJSIP_ETPNOTSUITABLE`. After the application returned, `timeout` terminated
the otherwise idle QEMU runner; that wrapper timeout is not the application
verdict.

The fresh Plan 1 capacity regression used:

```sh
cd /home/pitpapan/zephyrproject
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -p always -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-capacity-task7-final-restart -- \
  -DEXTRA_CONF_FILE=pjsua_capacity.conf \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-capacity-build.log

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout 55s west build -d /tmp/voip-plan3-capacity-task7-final-restart -t run \
  2>&1 | tee /tmp/voip-plan3-task7-final-restart-capacity-run.log
```

Across its warm-up and measured repetitions the transcript contained two
eighth-call 486 responses, fourteen 603 declines, and fourteen matching ACKs.
It ended with:

```text
PJSUA CAPACITY CLEANUP: baseline used=90192 live=32 cleanup used=90192 live=32
PJSUA CAPACITY ARENA: destroyed used=0 live=0 peak=274528
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
```

## Memory and stack evidence

The fresh build reports are QEMU stress-profile figures, not a production
memory budget:

| Profile | FLASH | RAM |
| --- | ---: | ---: |
| Plan 3 fake, final termination fix | 588,196 B / 4 MiB (14.02%) | 4,124,756 B / 4 MiB (98.34%) |
| Plan 3 real | 580,720 B / 4 MiB (13.85%) | 3,847,432 B / 4 MiB (91.73%) |
| Plan 1 capacity | 552,652 B / 4 MiB (13.18%) | 3,734,984 B / 4 MiB (89.05%) |

A read-only debugger run against the fresh real ELF stopped immediately before
and after each lifecycle's public shutdown. The five minimum-unused actor-stack
measurements were 20,840, 20,840, 20,840, 20,840, and 20,808 bytes. Before
shutdown, arena used bytes were 133,824 and live blocks were 64 each time;
peaks were 217,200, 217,200, 217,200, 217,200, and 214,544 bytes, with
high-water values 218,592, 218,592, 218,592, 218,592, and 216,496 bytes.
After every shutdown, arena used, peak, high-water, and live-block values were
all zero. Reset counts advanced from 2 through 6, and the corresponding wiped
byte counts were 218,592, 218,592, 218,592, 218,592, and 216,496.

The read-only debugger transcript is retained as
`/tmp/voip-plan3-task7-restart-gdb-values.log`. It was collected against the
same reviewed seven-source-file manifest used for this final restart; the final
restart recomputed that manifest unchanged after its profile runs. The command
was:

```sh
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb \
  -q -batch -x /tmp/voip-plan3-task7-restart-values.gdb \
  /tmp/voip-plan3-registration-task7-restart/zephyr/zephyr.elf \
  2>&1 | tee /tmp/voip-plan3-task7-restart-gdb-values.log
```

## Security and no-general-heap boundaries

The gate-fix component test proves the security boundary through the
production-selected `VoipService`, not merely through the generic registry.
For each separate public policy request—TLS signaling and SDES SRTP media—it
requires `unsupported_configuration`. Before each request it resets the narrow
PJSUA fake and snapshots the callback-router attach-attempt counter; after
rejection it requires all of the following: empty fake native sequence, zero
fake arena-install count, unchanged router attach-attempt count, null active
router, zero account adds, zero registrations, and zero active public agents.
This proves both rejected policies stop before arena installation and router
attach, as well as before native create, transport, account creation, and
registration. The fresh component log
`/tmp/voip-plan3-task7-final-restart-component-run.log` contains
`PjsuaPublicServiceTest PASSED` and the exact component terminator. The
generated fake and real configurations have SIP TLS, SRTP, PJMEDIA SRTP
transport, and SDES disabled; the runtime also sets `use_srtp` to
`PJMEDIA_SRTP_DISABLED`.

The real-profile general-heap gate is independent of the older host C++
allocation test. Only `CONFIG_VOIP_PJSUA_REGISTRATION_TEST` compiles
`GeneralHeapTracker.cpp` and supplies linker wrappers for `malloc`, `calloc`,
`realloc`, and `aligned_alloc` (with the target's lower-level allocation paths
also wrapped). Its pre-lifecycle self-test uses volatile function pointers and
requires each of the four libc entry points to increment the counter. The
recorded RED without interception is
`/tmp/voip-plan3-task7-heap-red.log`; it fails the self-test. With wrapping,
each successfully initialized five-account lifecycle starts a new counter only
after `Initialize()` and ends it after registration, polling, both 408/retry
paths, refresh, and ordered shutdown. Any nonzero allocation count fails the
lifecycle and suppresses the aggregate result.

The real registration evidence is
`/tmp/voip-plan3-task7-final-restart-registration-run.log`, which contains all five
lifecycle markers and exactly:

```text
VOIP PJSUA GENERAL HEAP: PASSED (0 allocations after initialization)
VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)
```

PJ pool blocks continue to use the private bounded arena; the measured reset
evidence above independently proves each completed native lifecycle returns
that storage to zero.

## Architectural exclusions

The committed Task 6 diff from `3a22a95881e84f124a78d369fed9e77aeb4b8faa`
through `2232c9c73` and the current tree were reviewed. The change set contains:

- no call manager or media-bridge implementation;
- no codec-priority policy;
- no SIP TLS or SRTP implementation (the actor's `errno` TLS probe tests
  thread-local storage integrity and is unrelated to SIP TLS);
- no PJSUA2 use;
- no public runtime account-add/remove/mutation API: the five accounts remain
  fixed at successful initialization;
- no legacy-stack deletion; and
- no `CallStateMachine`, `CallScheduler`, or `VoipEventQueue` path change, so
  call transitions, the two-global-promoted-call limit, and five-entry FIFO
  policy remain unchanged.

The subsequent `a6e629d67` commit changes only the Plan 1 capacity response
drain harness and its tests; it does not alter production VoIP behavior or any
capacity.

## Qualification limits

This acceptance is limited to host tests and QEMU `mps2/an385` fake, real
self-hosted loopback-registrar, and capacity profiles. The 2 MiB PJSUA arena,
1 MiB libc malloc arena, QEMU loopback interface, and in-process scripted
registrar are stress-test facilities and are not a production target memory or
network design.

RT1064 compile, hardware, and network-interface qualification were removed
from this continuation and remain future work. No RT1064 build, device run,
external registrar run, target-network selection, production memory-map
qualification, or embedded acceptance is claimed here.
