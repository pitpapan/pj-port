# VoIP Robustness, Migration, and Product Qualification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove bounded behavior under pressure and failure, complete safe shutdown, migrate validation users to the breaking API, remove the obsolete lowercase compatibility implementation after parity, and produce repeatable release evidence.

**Architecture:** A tiny always-enabled `VoipResourceGuard` maintains only admission-critical counters and invariants. Detailed PJ arena fragmentation, stack watermark, socket/timer inspection, and verbose dumps live in optional `PjsuaDiagnostics`, enabled for tests/qualification and absent from normal product builds. Qualification stresses the public service as a black box and validates resource baselines after every lifecycle.

**Tech Stack:** C++17, PJSUA-LIB, Zephyr/QEMU builds, fixed fault-injection adapters, scripted SIP/RTP peers, host concurrency tests, map/arena/stack measurements, Markdown acceptance records.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Business state machine:** `docs/superpowers/specs/state_machine.uml`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- Do not enable SIP TLS or SRTP in runtime tests or product profiles.
- Keep the TLS/SRTP enums, policies, and disabled Kconfig corners.
- Keep the always-on guard constant-time and allocation-free.
- Compile detailed diagnostics only with `CONFIG_VOIP_DIAGNOSTICS=y`; normal builds use `n`.
- Do not remove the uppercase/legacy product stack in this plan.
- Remove the lowercase compatibility facade/backend only after replacement parity and acceptance commands pass.
- No completion claim may rely only on an idle QEMU timeout; require explicit pass markers and measured cleanup.
- Qualification must cover the exact five-state business projection while
  independently checking private phase/resource invariants.
- A terminal transition is copied to the guaranteed event queue before
  immediate handle invalidation and slot reuse; no terminal-retention timer is
  permitted.

---

## Task 1: Add the minimal always-on resource guard

**Files:**

- Create: `voip/src/core/VoipResourceGuard.hpp`
- Create: `voip/src/core/VoipResourceGuard.cpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Create: `voip/tests/unit/VoipResourceGuardTest.cpp`

**Interfaces:**

- Consumes: every acquire/release transition for agents, calls, promoted slots, FIFO, commands, operations, events, and bridges.
- Produces: constant-time admission predicates, invariant faults, public `ResourceSnapshot` counters.

- [ ] Write tests that deliberately over-acquire each resource, release below zero, assign two promoted calls to one agent, exceed two global promoted calls, and create a bridge without a promoted call. Each must fail closed without corrupting the previous count.

- [ ] Confirm tests fail because the guard is absent.

- [ ] Implement fixed counters and per-agent promoted bits. Provide `TryAcquire*()`/`Release*()` methods and `ValidateInvariants()` with no iteration over dynamic storage and no PJPROJECT calls.

- [ ] Enforce exact maxima: agents 5, logical/native calls 7, promoted 2, pending 5, commands 16, operations 16, events 32 with one stopped reservation, bridges 2.

- [ ] On an invariant violation, stop new admissions, emit one reserved `resource_pressure`/internal-fault diagnostic if capacity permits, and begin controlled shutdown. Never continue by increasing a configured capacity or using heap fallback.

- [ ] Populate `ResourceSnapshot` from guard counters plus arena summary atomics. Do not make public snapshot reads walk PJSUA state.

- [ ] Run `VoipResourceGuardTest` and the existing scheduler/event tests; require success.

- [ ] Commit:

  ```sh
  git add voip/src/core/VoipResourceGuard.* voip/src/core/VoipRuntime.cpp voip/tests/unit/VoipResourceGuardTest.cpp
  git commit -m "feat(voip): enforce minimal runtime resource invariants"
  ```

## Task 2: Add optional test/qualification diagnostics

**Files:**

- Create: `voip/src/pjsua/PjsuaDiagnostics.hpp`
- Create: `voip/src/pjsua/PjsuaDiagnostics.cpp`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`
- Create: `voip/tests/pjsua/PjsuaDiagnosticsTest.cpp`

**Interfaces:**

- Consumes: arena stats, runtime-owned PJ counters/endpoints, actor/media stack watermark providers.
- Produces: optional detailed samples and qualification dump; zero production cost when disabled except existing guard counters.

- [ ] Write a diagnostics-enabled test that captures capacity/used/peak/largest-free/live-block/failure arena fields, active PJSUA accounts/calls, transport count, timer/transaction/dialog counts, actor stack watermark, and both media callback peaks.

- [ ] Add a compile test with diagnostics disabled proving no `PjsuaDiagnostics.cpp` symbol is linked and the public API still returns the minimal `ResourceSnapshot`.

- [ ] Confirm the enabled test fails because diagnostics are absent.

- [ ] Add `CONFIG_VOIP_DIAGNOSTICS` default `n`, dependent on `VOIP_SERVICE && VOIP_PJSUA`. Compile the diagnostics source only when it is `y`.

- [ ] Implement actor-only sampling at explicit test checkpoints, never per audio frame or per event-loop iteration. Copy results into a fixed record and sanitize all strings; diagnostics must never include credentials or SIP Authorization contents.

- [ ] Treat diagnostics as observational. It must not be required for admission, scheduling, teardown, or correctness; those remain guarded by `VoipResourceGuard`.

- [ ] Run both enabled and disabled tests and compare map output to confirm the optional object is absent when disabled.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaDiagnostics.* voip/zephyr voip/tests/pjsua/PjsuaDiagnosticsTest.cpp
  git commit -m "feat(voip): add optional PJSUA qualification diagnostics"
  ```

## Task 3: Make event/command pressure deterministic

**Files:**

- Modify: `voip/src/core/CommandMailbox.hpp`
- Modify: `voip/src/core/OperationTable.cpp`
- Modify: `voip/src/core/VoipEventQueue.cpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Create: `voip/tests/unit/VoipPressureTest.cpp`

**Interfaces:**

- Consumes: concurrent producers, slow event consumer, external incoming events.
- Produces: bounded rejection/coalescing and guaranteed terminal delivery.

- [ ] Write a deterministic test with four producer threads submitting commands while the sole polling consumer is paused. Fill commands, operations, ordinary events, and coalescible snapshots in controlled order.

- [ ] Verify immediate failures allocate no operation ID, accepted operations each emit exactly one terminal event, coalescible events replace snapshots, and no sequence number repeats.

- [ ] Inject an incoming call with no incoming-event reservation and verify 486/no context. Then request shutdown and verify the permanently reserved `service_stopped` record is the final produced event.

- [ ] Run the test against the implementation before pressure hardening and capture the first assertion failure.

- [ ] Fix reservation rollback, producer wakeups, coalescing keys, and terminal-event ownership without increasing capacities. Use one lock order documented in code: service lifecycle, command mailbox, operation table, event queue.

- [ ] Run the pressure test for 10,000 deterministic scheduling seeds and under thread sanitizer where the host PJ-independent subset supports it.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/tests/unit/VoipPressureTest.cpp
  git commit -m "test(voip): harden bounded command and event pressure"
  ```

## Task 4: Complete timeout-safe ordered shutdown

**Files:**

- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/VoipService.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaMediaManager.cpp`
- Modify: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Create: `voip/tests/integration/ShutdownTest.cpp`

**Interfaces:**

- Consumes: synchronous public `Shutdown()`, service-owned completion record, manager quiescence.
- Produces: ordered, idempotent shutdown or safe `shutdown_timeout` state.

- [ ] Write trace-based tests asserting the 14 approved shutdown stages in order: close admission; post owned shutdown command; cancel queued outgoing; reject queued incoming; tear down promoted calls/media; wait callbacks; unregister; drain; remove accounts/transport; destroy PJSUA/arena; erase credentials; invalidate handles; enqueue stopped; stop actor.

- [ ] Add failure/timeout tests at every asynchronous wait. After `shutdown_timeout`, verify storage remains alive, new public commands return `shutting_down`, and a later shutdown call can finish when the injected block clears.

- [ ] Add the regression for current `RunSync()` risk: time out the caller while the actor retains the command and prove no pointer targets caller stack memory.

- [ ] Confirm at least the timeout regression fails before the ordered shutdown implementation.

- [ ] Implement a service-owned shutdown command/completion record whose lifetime equals the `VoipService::Impl`. Do not free/reconstruct the implementation on timeout.

- [ ] Drain only teardown-required PJSUA events/timers; reject all new incoming calls during quiescence. Clear native user data before destroying PJSUA, then erase credential arrays with an optimization-resistant zeroing function.

- [ ] Preserve queued events after successful shutdown. Produce no event after `service_stopped`. Make repeated completed `Shutdown()` return `ok`.

- [ ] Run `ShutdownTest` for every injected stage and five clean lifecycle repetitions.

- [ ] Commit:

  ```sh
  git add voip/src voip/tests/integration/ShutdownTest.cpp
  git commit -m "fix(voip): make shutdown ordered and timeout safe"
  ```

## Task 5: Execute the full failure-injection matrix

**Files:**

- Create: `voip/tests/integration/FailureMatrixTest.cpp`
- Modify: `voip/src/pjsua/PjsuaApi.hpp`
- Modify: implementation files only where the test exposes a cleanup defect

**Interfaces:**

- Consumes: fake PJSUA/PJMEDIA return statuses and callback sequences.
- Produces: rollback proof for every owned acquisition/state transition.

- [ ] Enumerate named failure points for arena install, create/init/start, TCP transport, each of five account adds/registers, incoming 180, outgoing make-call, answer/reject/hangup/hold/re-INVITE, media transport, custom port add, each conference connect, disconnect/remove, unregister/delete, and PJSUA destroy.

- [ ] For each call-control failure point record the private phase, projected
  source/destination `CallState`, transition cause, `HoldReason`, terminal-event
  reservation, and resource counts. Reject any impossible projection such as
  `hold(waiting)` owning a media bridge or `hold(media)` lacking a promoted
  lease; allow private promoted phases while the public state remains
  `hold(waiting)` pending acceptance.

- [ ] For each point, write expected public error/event, expected reverse cleanup trace, and final resource baseline. Include duplicate, missing, reordered, and late callback variants.

- [ ] Run the matrix and record failures before changing implementation.

- [ ] Fix one ownership defect at a time with the smallest implementation change and rerun the failing row plus all earlier rows.

- [ ] Require every row to finish with no occupied logical/native context, promoted lease, FIFO entry, operation/event reservation, bridge, account, transport, callback, or live arena block beyond the explicitly documented initialized baseline.

- [ ] Run with AddressSanitizer/UndefinedBehaviorSanitizer for the host fake subset and require zero findings.

- [ ] Commit:

  ```sh
  git add voip/tests/integration/FailureMatrixTest.cpp voip/src
  git commit -m "test(voip): cover complete ownership failure matrix"
  ```

## Task 6: Qualify worst-case memory, fragmentation, stacks, and soak

**Files:**

- Create: `applications/voip_integration/src/product_qualification.cpp`
- Create: `applications/voip_integration/product_qualification.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Create: `docs/voip/qualification-report.md`

**Interfaces:**

- Consumes: diagnostics-enabled full service and worst-case scripted peers.
- Produces: measured QEMU/target resource evidence and pass/fail thresholds.

- [ ] Start QEMU qualification with `CONFIG_PJSUA_ARENA_BYTES=2097152` and diagnostics enabled. Create five registered agents, two simultaneous promoted media calls, and five queued incoming calls; concurrently fill command/event coalescing paths without violating guaranteed reservations.

- [ ] Sample initialized baseline, worst-case steady state, every teardown stage, and post-destroy state. Record arena used/peak/largest free/failures, static RAM/ROM, active socket/timer/transaction/dialog counts, and actor/media stack watermarks.

- [ ] Run normal worst-case cycling long enough for at least 10,000 call lifecycles and 24 hours, whichever is longer in the qualification environment. Inject registration reconnect, remote CANCEL, RTP loss/recovery, hold/resume, queue timeout, and slow event consumption.

- [ ] Enforce spec thresholds: normal soak arena usage at or below 75%; failure scenarios below 90%; zero allocation fallback/failure in normal soak; resource counts return to baseline every cycle; no monotonic peak/live-block growth after warm-up.

- [ ] Build/run QEMU:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_qualification -- \
    -DEXTRA_CONF_FILE=product_qualification.conf
  west build -d build_voip_qualification -t run
  ```

  Expected final marker after the configured soak: `VOIP QUALIFICATION RESULT: PASSED`.

- [ ] For target qualification, require CI to set the product-specific variable and fail if absent, then run the same profile:

  ```sh
  test -n "${VOIP_QUAL_BOARD:?VOIP_QUAL_BOARD must name the approved product board}"
  west build -p always -b "$VOIP_QUAL_BOARD" applications/voip_integration \
    -d build_voip_qualification_target -- \
    -DEXTRA_CONF_FILE=product_qualification.conf
  ```

- [ ] Select the smallest aligned target arena whose measured worst-case peak remains below 75%; record the chosen size and evidence in `qualification-report.md`. If it exceeds the approved target RAM budget, record the architecture gate as failed and stop PJSUA adoption rather than enabling heap fallback.

- [ ] Commit:

  ```sh
  git add applications/voip_integration docs/voip/qualification-report.md
  git commit -m "test(voip): add product resource qualification"
  ```

## Task 7: Migrate validation users and remove lowercase compatibility code

**Files:**

- Replace: `applications/voip_integration/src/sdk_contract.cpp`
- Replace or remove after parity: `applications/voip_integration/src/main.cpp`
- Replace or remove after parity: `applications/voip_integration/src/phase2_runtime.cpp`
- Replace or remove after parity: `applications/voip_integration/src/phase3_account.cpp`
- Replace or remove after parity: `applications/voip_integration/src/phase4_registration.cpp`
- Replace or remove after parity: `applications/voip_integration/src/phase5_call.cpp`
- Replace or remove after parity: `applications/voip_integration/src/phase6_media.cpp`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Modify: `applications/voip_integration/Kconfig`
- Modify or remove after parity: `applications/voip_integration/prj.conf`
- Modify or remove after parity: `applications/voip_integration/sdk_contract.conf`
- Modify or remove after parity: `applications/voip_integration/phase2_runtime.conf`
- Modify or remove after parity: `applications/voip_integration/phase3_account.conf`
- Modify or remove after parity: `applications/voip_integration/phase4_registration.conf`
- Modify or remove after parity: `applications/voip_integration/phase5_call.conf`
- Modify or remove after parity: `applications/voip_integration/phase6_media.conf`
- Modify or remove after parity: `applications/voip_integration/phase7_media.conf`
- Modify or remove after parity: `applications/voip_integration/phase8_hold.conf`
- Modify or remove after parity: `applications/voip_integration/phase9_robustness.conf`
- Modify or remove after parity: `applications/voip_integration/srtp_keys.conf`
- Modify or remove after parity: `applications/voip_integration/srtp_call.conf`
- Remove after parity: `voip/include/voip/VoipFacade.hpp`
- Remove after parity: `voip/include/voip/FakeVoipBackend.hpp`
- Remove after parity: `voip/include/voip/PjVoipBackend.hpp`
- Remove after parity: `voip/src/FakeVoipBackend.cpp`
- Remove after parity: `voip/src/VoipManager.cpp`
- Remove after parity: `voip/src/PjVoipBackend.cpp`
- Remove after parity: `voip/src/PjRuntime.hpp`
- Remove after parity: `voip/src/PjRuntime.cpp`
- Remove after parity: `voip/src/SipManager.hpp`
- Remove after parity: `voip/src/SipManager.cpp`
- Remove after parity: `voip/src/RtpManager.hpp`
- Remove after parity: `voip/src/RtpManager.cpp`
- Remove after parity: `voip/src/PjHeadlessMedia.hpp`
- Remove after parity: `voip/src/PjHeadlessMedia.cpp`
- Remove after parity: `voip/src/PjSrtpKeyMaterial.hpp`
- Remove after parity: `voip/src/PjSrtpKeyMaterial.cpp`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`

**Interfaces:**

- Consumes: approved replacement tests/plans and old validation behavior.
- Produces: one honest lowercase VoIP API/implementation; preserved legacy product stack outside this directory.

- [ ] Create a parity checklist mapping old lifecycle, registration, call, media, hold, robustness, and disabled-security validation to the new Plan 2–6 tests. Every old behavior retained by the approved spec must have a passing replacement command.

- [ ] Migrate `sdk_contract.cpp` first to construct `VoipService`, initialize agent/audio arrays, retrieve agent handles, submit asynchronous operations, and poll terminal/state events. Confirm it fails to compile before migration and passes afterward.

- [ ] Replace old phase test selection with the new link, arena, capacity, multi-account, multi-call, multi-audio, and qualification profiles. Keep old validation sources buildable until every replacement pass marker is recorded.

- [ ] Run the full replacement host and QEMU suite. Archive exact commands/pass markers in the qualification report.

- [ ] Only after parity passes, remove the listed lowercase compatibility headers/sources and their Kconfig symbols: `VOIP_FACADE`, `VOIP_FAKE_BACKEND`, `VOIP_PJ_BACKEND`, `VOIP_PJ_REGISTRATION_NETWORK`, `VOIP_PJ_CALL_CONTROL`, `VOIP_PJ_HEADLESS_MEDIA`, and old active SRTP facade gates.

- [ ] Preserve `CONFIG_VOIP_SIP_TLS` and `CONFIG_VOIP_SRTP` as disabled replacement extension gates. Do not compile or run their implementations in the initial product configuration.

- [ ] Verify the repository has no lowercase application references to `VoipManager`, `Backend`, `PjVoipBackend`, or `EventHandler`:

  ```sh
  rg -n "VoipManager|PjVoipBackend|class Backend|EventHandler" voip applications/voip_integration
  ```

  Expected: no matches.

- [ ] Build the separate legacy product stack using its documented existing build command without modifying or inspecting Zephyr source. Record that it remains available; retirement requires a separate approved change.

- [ ] Commit:

  ```sh
  git add voip applications/voip_integration docs/voip/qualification-report.md
  git commit -m "refactor(voip): retire lowercase compatibility backend"
  ```

## Task 8: Final architecture and release review

**Files:**

- Create: `docs/voip/release-checklist.md`
- Modify: `docs/voip/qualification-report.md`
- Modify documentation only for discrepancies proven by implementation

**Interfaces:**

- Consumes: all six plan exit criteria and test evidence.
- Produces: reviewable go/no-go decision without enabling deferred security.

- [ ] Run all host tests, pristine QEMU profiles, target qualification, `git diff --check`, and the documented legacy-stack build. Record command, date, revision, configuration, final marker, and measured resource result for each.

- [ ] Review public headers for PJ/Zephyr workqueue leakage, credential exposure, runtime topology mutation, callbacks, unsupported codec policy, and accidental TLS/SRTP activation.

- [ ] Review source for `new`, `delete`, `malloc`, `free`, unbounded containers, worker thread creation, globs in PJ source manifests, and calls into PJSUA outside the actor. Exempt only initialization-time placement construction and the fixed arena implementation documented by the design.

- [ ] Verify worst-case invariants from diagnostics: 5 agents, 7 logical/native contexts, 2 promoted, 5 pending, 2 bridges, one promoted per agent, zero resource drift, and stopped event last.

- [ ] Replay every edge in `state_machine.uml`. Verify FIFO wait and media hold
  are distinguished, terminal events precede handle invalidation, old handles
  fail immediately after cleanup, and the copied terminal event remains
  readable without occupying a logical slot.

- [ ] Mark release `GO` only if every required test passes, target arena remains within budget/thresholds, no post-init general heap use is observed, and plain-security deployment risk is explicitly accepted. Otherwise mark `NO-GO` with the exact failed criterion; do not weaken the architecture automatically.

- [ ] Commit:

  ```sh
  git add docs/voip/release-checklist.md docs/voip/qualification-report.md
  git commit -m "docs(voip): record multi-agent release decision"
  ```

## Plan 6 Exit Criteria

- Minimal resource invariants are always enforced without detailed-monitor overhead.
- Every business-state UML edge and invalid transition is qualified against
  independent private phase/resource invariants.
- Detailed PJSUA/arena/stack monitoring is test/qualification-only and compiled out by default.
- Pressure, concurrency, failures, timeouts, late callbacks, and shutdown are deterministic and bounded.
- QEMU and approved target qualification meet the 75%/90% arena thresholds and return resources to baseline.
- Validation users use the breaking polling API.
- Obsolete lowercase facade/backend code is removed only after replacement parity.
- TLS/SRTP have configuration/policy corners but remain disabled and unused.
- The separate legacy product stack remains available pending a later explicit retirement decision.
