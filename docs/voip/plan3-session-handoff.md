# Plan 3 session handoff

**Prepared:** 2026-09-05

**Repository:** `/home/pitpapan/zephyrproject`

**Worktree:** `/home/pitpapan/zephyrproject/.worktrees/pjsua-plan3`

**Branch:** `codex/pjsua-plan3`
**Remote baseline:** `origin/codex/pjsua-plan3` at `3a22a9588` (`voip: guard unsupported PJSUA calls`)

This document is the source of truth for resuming Plan 3 in another session. The branch contains substantial **uncommitted** Task 6 and Task 7 work. Do not discard, reset, or overwrite it.

## Non-negotiable workspace rule

Do not inspect, search, index, or modify anything under the top-level `zephyr/` directory. Treat it as an external platform dependency.

Allowed operations include documented `west` commands and reading compiler, linker, QEMU, and GDB output. Work may inspect and modify our `applications/`, `voip/`, `pjproject/`, and `docs/` trees. If a problem genuinely requires reading Zephyr implementation source, stop and ask for explicit authorization for that specific file.

## Read these documents first

- `designIdead.md`
- `reviews.md`
- `state_machine.uml`
- `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`
- `docs/superpowers/plans/2026-08-27-pjsua-five-account-registration.md`
- `docs/superpowers/plans/2026-09-05-pjsua-plan3-task6-continuation.md`

The user prefers the same execution flow used so far: a Terra-medium subagent implements a bounded task, and the primary agent reviews and validates it before accepting the change. The continuation plan supersedes the original monolithic Task 6 with Tasks 6A-6F. There is no Task 8; RT1064 qualification is deferred from this continuation.

## Product constraints that must remain true

- One service and one actor own one PJSUA runtime.
- Up to five agents are configured only during initialization.
- Each agent has at most one active call.
- The system retains the approved two-global-promoted-call limit and five-entry FIFO queue. Plan 3 must not change the call state machine or queue policy.
- Each agent owns independent registration state and audio bindings; `PjsuaAccountContext` must not own credentials or audio.
- PJSUA and PJMEDIA worker counts stay zero. Only the existing actor polling loop calls `pjsua_handle_events()`.
- Plain SIP TCP is the only transport used now. TLS and SRTP remain visible policy corners but disabled and unimplemented.
- Codec selection, call management, media bridges, and PJSUA2 remain out of Plan 3.
- No general-heap use is allowed after successful initialization.
- The intended embedded target is `mimxrt1064_evk`; QEMU stress-profile memory figures must not be represented as a production RT1064 budget.

## Completed and pushed work

Plan 3 Tasks 1 through 5 were completed, reviewed, committed, and pushed. The latest pushed commit is:

```text
3a22a9588 voip: guard unsupported PJSUA calls
```

Earlier accepted Plan 3 commits, oldest to newest:

```text
f5e098e9c
ed678f04d
2952c8d11
7115311fd
0a5d46d6a
1006f3c41
8b08e243d
9907e78ca
ca5e71b85
816125cd5
541c90b94
2696de2f1
811141f94
2dddd3c38
aa4ffa918
68b8aac02
524e7435a
3a22a9588
```

Task 5 previously passed the ten-test host suite and the PJSUA component profile. Those results predate the current uncommitted Task 6/7 changes and must not be used as final verification.

## Current plan state

- Tasks 1–5: complete and pushed.
- Task 6, five-account real PJSUA registration scenario: in progress and uncommitted.
- Task 6A-6E: independently reviewed and accepted without partial commits.
- Task 6F implementation and fresh real/fake verification: complete; complete-
  Task-6 review fix round 1 restores the accepted 6B late-event boundary and
  awaits scoped re-review. The single Task 6 commit remains controller-owned.
- Task 7, regression/embedded/acceptance gate: partially implemented and uncommitted.
- No current uncommitted work has been pushed.

At handoff, `git diff --check` passed. Approximately 31 tracked files are modified with about 539 insertions and 41 deletions, plus the new registration scenario files.

## Uncommitted change groups

### Five-account registration integration

New files:

```text
applications/voip_integration/pjsua_registration.conf
applications/voip_integration/src/pjsua_registration.cpp
applications/voip_integration/test_support/ScriptedRegistrar.hpp
applications/voip_integration/test_support/ScriptedRegistrar.cpp
```

`CMakeLists.txt` and `Kconfig` select the scenario. It installs a test-only PJSIP registrar module on the same PJSUA endpoint and loopback TCP listener. Production public headers do not expose the native PJSIP endpoint.

The scenario intends to prove:

- five configuration-order handles with distinct mandatory audio bindings;
- four valid Digest registrations and one final 403 for the bad password;
- isolated one-shot 408 failures and deterministic retries for agents 1 and 3;
- refresh for at least two accounts;
- TCP-only traffic and zero worker threads;
- shutdown while a retry is pending;
- five complete initialize/register/retry/refresh/shutdown lifecycles;
- no native accounts, no active transport/router, null PJSUA state, invalid handles, and zero arena accounting after each lifecycle;
- exact final marker:

```text
VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)
```

That marker was reached exactly once after five numbered complete lifecycle
markers in `/tmp/voip-plan3-registration-6f-final-run.log`. The implementation
is not committed and Task 6 is not accepted until the controller completes the
independent whole-Task-6 review and staged-diff checkpoint.

### PJLIB external-thread registration

`PjsuaApi` now exposes `pj_thread_is_registered()` and `pj_thread_register()` through the native/fake function table. `PjsuaRuntime` owns the external actor thread descriptor and checks registration before PJSUA calls.

This fixed the original `pj_thread_this()` assertion reached from `pjsua_handle_events()`. The actor is a Zephyr-created thread, not a PJ-created worker, so explicit registration is required.

### Proven actor-stack underflow

The original 8 KiB PJSUA actor stack corrupted `k_thread.tls` while executing:

```text
pjsua_acc_add
  -> pj_log
  -> pj_gettimeofday
  -> gettimeofday/sys_clock_gettime
  -> __udivmoddi4
```

Fresh GDB hardware-watchpoint evidence showed the saved actor TLS field being overwritten below the stack boundary. A 16 KiB diagnostic still failed. The PJSUA configuration now defaults to a fixed 32 KiB actor stack; lightweight adapters retain 8 KiB. The code also uses the matching `K_KERNEL_STACK_SIZEOF()` macro for `K_KERNEL_STACK_MEMBER`.

A registration-only TLS probe writes a sentinel to `errno`, forces a context switch, verifies it afterward, and records minimum unused actor stack. This probe and its reporting still require final review and validation across all five lifecycles.

### Transactional startup and teardown

The architecture audit found and changes attempt to fix three high-severity bugs:

1. Failed PJ thread registration during startup rollback could leave PJSUA/router/arena alive while initialization reported failure.
2. `pjsua_destroy()` failure was ignored before arena reset/router detach.
3. Startup cleanup was attempted only a bounded number of times before the actor could be stopped.

The current implementation adds a `startup_cleanup` lifecycle. The actor retries adapter shutdown until native teardown succeeds, then reports the original startup failure. Runtime destroy retains native/arena state after registration or native-destroy failure. Fake tests cover several registration failures followed by recovery, exact teardown order, and destroy failure followed by retry.

These combined changes passed the fresh full component profile against the
complete current diff in `/tmp/voip-plan3-component-6f-run.log`. Review the
timeout/destructor behavior carefully: native callback-owned state must never
be freed, even if cleanup keeps failing.

### Correct retry clock ordering

The audit found that native callbacks ran before `PjsuaAccountManager` received the current actor timestamp. A 408 could therefore schedule from stale time and retry immediately.

The adapter now calls `accounts_.SetNow(now_ms)` before native callback dispatch and processes due timers afterward. The component test injects failure at time 5000 and requires no retry through 5999, then retry at 6000. Revalidate the full 1, 2, 4, ... second bounded schedule plus deterministic slot jitter.

### Credential and arena zeroization

`AgentRegistry::CredentialsErasedForTest()` reports only whether internal username/password byte ranges are zero; it never exposes secret values. Host coverage checks reset and failed initialization.

The private PJ arena now tracks the highest reachable byte, enters an explicit resetting state, releases the spinlock, zeroizes the reachable region with volatile writes, then reinitializes metadata. Reset/install/stat/allocation paths reject conflicting access while resetting. Wipe byte/count fields were added to private arena statistics, and the integration scenario checks that a nonzero reachable region was wiped.

Important: this logic was revised because wiping an entire 2 MiB arena while holding `k_spin_lock` would hold interrupts for too long. The revised state-machine/lock behavior received the Task 6E independent review and passed the fresh Task 6F real/fake profiles; the broader Task 7 regression gate is still pending.

### RT1064 placement alignment

The first `mimxrt1064_evk` compile exposed:

```text
static_assert(alignof(std::max_align_t) >= alignof(Impl))
ARM comparison: 8 >= 64
```

`VoipService` now uses fixed `alignas(64)` in-object storage and asserts the actual service alignment against `Impl`. This resolves compilation of `VoipService.cpp` on that target without adding a heap fallback. A complete RT1064 link and memory report has not yet been obtained.

## Current Task 6 review checkpoint

Task 6F now composes authentication, isolated initial 408 recovery for agents 1
and 3, independent refresh for agents 0 and 2, a new pending refresh retry for
agent 1 at shutdown, ordered teardown, invalidated handles, empty resources,
and arena zeroization into each of exactly five complete lifecycles. One static
`VoipService` is reused; only scripted registrar state is reset between
lifecycles. Successful-lifecycle and arena reset-wipe counters advance
monotonically. A lifecycle failure suppresses the exact final marker.

The separate Task 6E early shutdown timing diagnostic is not a sixth real
lifecycle. Its causal busy-unregister behavior remains in focused fake coverage;
the fresh component log prints
`PjsuaRuntimeAdapterTest outstanding REGISTER retains shutdown barrier: yes`.

Fresh Task 6F real evidence:

```text
Build: /tmp/voip-plan3-registration-6f-final-build.log (exit 0)
Run:   /tmp/voip-plan3-registration-6f-final-run.log (application complete at 42.190s; idle-QEMU timeout exit 124)
FLASH: 577,988 bytes / 4 MiB (13.78%)
RAM:   3,846,896 bytes / 4 MiB (91.72%)
VOIP PJSUA LIFECYCLE 1: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 2: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 3: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 4: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA LIFECYCLE 5: PASSED (auth, 408 recovery, refresh, pending retry shutdown)
VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)
```

The source fingerprints in
`/tmp/voip-plan3-registration-6f-source-before.sha256` and
`/tmp/voip-plan3-registration-6f-source-after.sha256` match, proving the final
build/run used one stable source revision. The real log has no application
FAIL, abort, Usage Fault, or `PJSIP_ETPNOTSUITABLE` signature.

Fresh fake component evidence:

```text
Build: /tmp/voip-plan3-component-6f-build.log (exit 0)
Run:   /tmp/voip-plan3-component-6f-run.log (timeout exit 124)
PJSUA PLAN 3 COMPONENT RESULT: PASSED
***** USAGE FAULT *****
  Illegal load of EXC_RETURN into PC
```

The Usage Fault remains the inherited post-marker main-thread termination
fault. Clean component QEMU termination is not claimed. Source checks found no
`[PJSUA registration debug]` or `debug_turns`; `git diff --check` passed; no
call-state, scheduler, or FIFO file changed.

The controller must now review the complete Task 6 diff against the approved
architecture, `designIdead.md`, `reviews.md`, and the state machine. Only after
independent approval should it stage the exact Task 6 paths, run
`git diff --cached --check`, inspect the staged diff, and create the single
Task 6 commit. No Task 6F commit has been made.

### Complete Task 6 review fix round 1

The first complete-Task-6 review found no Critical, architecture, call-state,
FIFO, or teardown issue, but rejected one Important acceptance-test gap. The
initial Task 6F composition stopped at its active predicates and did not repeat
all exact authentication/retry counts after the post-shutdown event drain. A
late extra registration outcome, including a forbidden agent 4 registration,
could therefore escape the final lifecycle boundary.

The fix restores an explicit zero-registration check for agent 4 after initial
recovery and, after shutdown drains every remaining public event, requires the
complete exact outcome matrix: registered counts `[2,1,2,1,0]`, transport-
failure and retry-wait counts `[0,2,0,1,0]`, no authentication failure for
agents 0-3, and exactly one final 403 authentication failure for agent 4. This
is the composed-lifecycle equivalent of the accepted 6B real-time settling
window: retaining that nine-second window before shutdown would deliberately
advance the refresh/retry behavior being composed, while the final drain sees
and rejects every late queued outcome. No existing assertion was weakened.

Fresh review-fix evidence uses:

```text
Real build: /tmp/voip-plan3-registration-6f-fix1-build.log (exit 0)
Real run:   /tmp/voip-plan3-registration-6f-fix1-run.log (five lifecycle markers, one exact terminator, then idle-QEMU timeout exit 124)
Fake build: /tmp/voip-plan3-component-6f-fix1-build.log (exit 0)
Fake run:   /tmp/voip-plan3-component-6f-fix1-run.log (exact component marker, inherited post-marker Usage Fault, timeout exit 124)
```

For both fresh profiles, SHA-256 manifests cover all 39 paths in
`task-6-complete-review-files.txt`, including the handoff and complete tracked/
untracked Task 6 source set. Each before/after pair is identical:

```text
/tmp/voip-plan3-task6-6f-fix1-real-before.sha256
/tmp/voip-plan3-task6-6f-fix1-real-after.sha256
/tmp/voip-plan3-task6-6f-fix1-fake-before.sha256
/tmp/voip-plan3-task6-6f-fix1-fake-after.sha256
```

The real log retains exactly five numbered lifecycle markers and one exact
final terminator with no application FAIL, abort, Usage Fault, or
`PJSIP_ETPNOTSUITABLE` signature. The fake log retains the focused busy-
unregister regression and exact component marker before the already-qualified
main-thread termination Usage Fault. Debug search and `git diff --check` remain
clean. Scoped re-review and the controller-owned staging/commit checkpoint are
still pending; no fix-round commit was made.

## Disproved hypotheses

Do not repeat these investigations unless new evidence contradicts them:

- The fault was not caused by a truncated socket address pointer; initial accept arguments were valid arena addresses.
- A 16 KiB actor stack did not prevent the TLS corruption.
- Switching from `K_THREAD_STACK_SIZEOF` to the matching `K_KERNEL_STACK_SIZEOF` was a correctness fix but was not the cause of the crash.
- The fatal TLS write was not the first expected EWOULDBLOCK errno write during listener setup. It occurred later after stack underflow had corrupted the saved TLS field.
- The post-stack-fix registration problem is not a public `WaitForEvent` timeout/scheduler failure; the application thread continues to advance.

## RT1064 qualification status (deferred)

The user removed RT1064 qualification from the Plan 3 continuation. This section preserves already-discovered constraints for future work; it is not an active gate and must not be represented as completed.

The available board target is:

```text
mimxrt1064_evk
```

The generated board configuration seen during the compile attempt reported 32 MiB SRAM and 4 MiB flash with XIP. Do not equate that configured region with an approved production/internal-SRAM memory map.

The full self-hosted QEMU registration profile is not an RT1064 hardware profile:

- it reserves a 2 MiB PJSUA arena, 1 MiB libc malloc arena, 256 KiB heap, 64 KiB main stack, and 32 KiB actor stack;
- it relies on loopback and the in-process scripted registrar;
- the board configuration did not provide a selected network driver/interface.

When embedded qualification resumes, its first gate is compile/link of `pjsua_registration_fake.conf`. A real board runtime gate needs a product-selected Ethernet interface and external plain-TCP registrar. Do not invent that board/network choice. The architecture requires measuring native arena peak, choosing the smallest aligned arena whose peak is at most 75%, and rejecting configurations over 90%.

## Required next steps

Follow `docs/superpowers/plans/2026-09-05-pjsua-plan3-task6-continuation.md` sequentially:

1. Tasks 6A-6E are independently reviewed and accepted without partial commits.
2. Task 6F implementation and complete-review fix round 1 verification are complete.
3. Scoped re-review the Important acceptance-test fix, then stage only the exact
   Task 6 paths, verify the staged diff, and make the single Task 6 commit after approval.
4. Task 7: run the reduced regression gate, create the acceptance record, and commit it.

There is no Task 8. Do not add RT1064 compile or hardware qualification to this continuation; record it as deferred.

## Suggested fresh build commands

Run from `/home/pitpapan/zephyrproject`.

### Real five-account registration profile

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-registration-resume -- \
  -DEXTRA_CONF_FILE=pjsua_registration.conf

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
west build -d /tmp/voip-plan3-registration-resume -t run
```

### Plan 3 fake component profile

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-component-resume -- \
  -DEXTRA_CONF_FILE=pjsua_registration_fake.conf

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
west build -d /tmp/voip-plan3-component-resume -t run
```

Expected marker:

```text
PJSUA PLAN 3 COMPONENT RESULT: PASSED
```

### Plan 1 capacity regression

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 \
west build -b mps2/an385 \
  -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
  -d /tmp/voip-plan3-capacity-resume -- \
  -DEXTRA_CONF_FILE=pjsua_capacity.conf

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
west build -d /tmp/voip-plan3-capacity-resume -t run
```

Expected marker:

```text
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
```

### Host gate

```sh
cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
bash voip/tests/run_host_tests.sh
bash voip/tests/run_host_tests.sh
```

## Current worktree file inventory

Tracked files were modified in these areas:

```text
applications/voip_integration/{CMakeLists.txt,Kconfig,src/pjsua_arena_test.c}
pjproject/zephyr/{include/pj_zephyr_pool_arena.h,pj_zephyr_pool_arena.c}
voip/include/voip/VoipService.hpp
voip/src/{HandlePool.hpp,VoipService.cpp}
voip/src/core/{AgentRegistry,CoreActor,VoipRuntime}.{cpp,hpp}
voip/src/pjsua/PjsuaAccountManager.{cpp,hpp}
voip/src/pjsua/PjsuaApi.{cpp,hpp}
voip/src/pjsua/PjsuaCallbackRouter.{cpp,hpp}
voip/src/pjsua/PjsuaRuntime.{cpp,hpp}
voip/src/pjsua/PjsuaRuntimeAdapter.{cpp,hpp}
voip/src/pjsua/PjsuaTransportManager.cpp
voip/tests/pjsua/{FakePjsuaApi,PjsuaPublicServiceTest,PjsuaRuntimeAdapterTest,PjsuaRuntimeTest}.*
voip/tests/unit/AgentRegistryTest.cpp
voip/zephyr/Kconfig
```

Untracked source files are the registration profile/scenario/test-support files listed earlier. This handoff file is also new and untracked until committed.

## Definition of done

## Task 6A diagnostic record (2026-09-05)

Fresh profile reproduction was captured in
`/tmp/voip-plan3-registration-6a.log`: TCP accept completed, but the
registrar did not receive the request and the actor did not produce the
expected registration state. `west debug` could not be used for this QEMU
build because it has no `runners.yaml`; QEMU was therefore run with its local
GDB stub, using the command emitted by the documented `west build -t run`
target.

The live actor stack was:

```text
pjsua_handle_events(0)
  -> PjsuaRuntime::Pump(0)
  -> PjsuaRuntimeAdapter::Pump(now, 0)
  -> VoipRuntime::Step
  -> CoreActor::Entry
```

This was not a lock wait or a blocking native frame. `pjsua_handle_events()`
passed `{sec=0,msec=0}` to `pjsip_endpt_handle_events2()`, and
`pj_ioqueue_poll()` returned `-120012`. The latter was observed with
`ioqueue->nfds == 5`; PJSIP therefore called `select(6, ...)`. Status 120012
is `PJ_ERRNO_START_SYS + ENOMEM`. The generated registration profile had
`CONFIG_ZVFS_OPEN_MAX=0` and `CONFIG_ZVFS_POLL_MAX=3`, unlike the established
network profiles. Setting this test profile only to `OPEN_MAX=32` and
`POLL_MAX=24` fixed event delivery.

The permanent one-account smoke then exposed a separate RED: registration
completed but a literal single `Shutdown()` returned `shutdown_timeout`. GDB
showed clean unregistration (200) followed by `PjsuaRuntime::Destroy()`.
PJPROJECT's `pjsua_destroy2()` deliberately performs
`busy_sleep(1000 - i*50)` after unregistration; the observed native path took
about 1.5 seconds. The public runtime had a hard 1000 ms wait, so it could not
meet the required bounded single-call smoke contract. The private bounded wait
is now 3000 ms; no public API, worker count, actor polling ownership, or
native teardown order changed.

Final smoke evidence is `/tmp/voip-plan3-registration-6a-final.log`; it
contains exactly:

```text
VOIP PJSUA REGISTRATION SMOKE: PASSED (1 account, TCP Digest, actor polling)
```

The component gate now reaches `PJSUA PLAN 3 COMPONENT RESULT: PASSED` in
`/tmp/voip-plan3-component-6a-green-final.log`. GDB proved registration is
correctly copied into the public snapshot. The earlier line-108 abort location
was a shared optimized assertion branch: the actual failing assertion was the
later startup-cleanup `GetAgentHandle(0)` check, which expected `invalid_handle`
instead of the established out-of-range-index result `invalid_argument`.
That test expectation is corrected, with explicit checks for an invalid output
handle and zero active agents. Both experimental waiting changes were removed.
No production publication change was needed. Real smoke was reconfirmed in
`/tmp/voip-plan3-registration-6a-confirmed.log`.

Task 6A implementation status is DONE_WITH_CONCERNS pending primary review.
After the component marker, the image reports an EPSR usage fault with PC zero.
A watchpoint proves main's saved return address stays intact through all tests;
the fault follows normal main return and the platform main-thread termination
path. No platform/app exit behavior was changed, and clean component process
termination is not claimed. Full RED/GREEN, commands, state/return traces, and
inspection-boundary details (including the initial automatic GDB reset-source
line display before source resolution was disabled) are recorded in
`.superpowers/sdd/2026-09-05-pjsua-plan3-task6-continuation/task-6A-report.md`.

Plan 3 is not complete until Task 6’s exact five-lifecycle marker, both Plan 3 profiles, the host suite twice, the capacity regression, no-heap gate, credential/arena cleanup checks, final independent review, and the Task 7 acceptance record all pass against the same stable source revision. RT1064 compile, hardware networking, and production memory-map qualification are explicitly deferred and must not be represented as completed.
