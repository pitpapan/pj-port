# Plan 3 Task 6 Continuation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish Plan 3's real five-account PJSUA registration proof by resolving the native event-pump hang through evidence, then adding authentication, retry, refresh, teardown, and repeated-lifecycle assertions in independently reviewable stages.

**Architecture:** Keep one `VoipService`, one actor, one PJSUA runtime, one callback router, and one plain-TCP transport. The scripted registrar remains a test-only PJSIP module on the PJSUA endpoint; each stage exercises the public polling API and adds only one behavior dimension after the preceding stage passes.

**Tech Stack:** C++17, PJSUA-LIB/PJSIP C APIs, PJPROJECT's fixed Zephyr pool arena, Zephyr actor thread, SIP TCP Digest authentication, host fake-function-table tests, `mps2/an385` QEMU, and GDB.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Continuation state:** Read `docs/voip/plan3-session-handoff.md` and `docs/superpowers/plans/2026-08-27-pjsua-five-account-registration.md`. Tasks 1-5 are pushed through `3a22a9588`; the inherited Task 6/7 worktree is intentionally uncommitted and must be preserved.

## Supersession and scope

This document supersedes only Task 6 of `2026-08-27-pjsua-five-account-registration.md`. That task is decomposed into Tasks 6A-6F below. The original Task 7 becomes the reduced final gate in this document.

There is deliberately **no Task 8**. RT1064 compile, hardware networking, and production memory-map qualification are deferred from this continuation. The implementation must remain compatible with the intended `mimxrt1064_evk` target, but this plan must neither run nor claim that qualification.

## Global constraints

- Never inspect, search, index, or modify the top-level `zephyr/` directory. Use documented Zephyr interfaces and build output only.
- Configure one to five agents only during `VoipService::Initialize()`; no runtime account mutation.
- Permit at most one active call per agent. Preserve the two-global-promoted-call limit and five-entry FIFO queue unchanged.
- Keep audio binding ownership in `AgentRegistry`; `PjsuaAccountContext` owns neither credentials nor audio.
- Keep PJSUA and PJMEDIA worker counts at zero. Only the core actor calls `pjsua_handle_events()`.
- Use plain SIP TCP. TLS and SRTP remain visible, disabled policy corners.
- Do not add call management, media bridges, codec policy, PJSUA2, or legacy-stack deletion.
- Do not expose PJPROJECT types or the native endpoint through the public service API.
- Do not use the general heap after successful initialization.
- Never print credentials, Authorization headers, Digest responses, or other secrets.
- Do not weaken `InitialComplete`, `RefreshObserved`, teardown assertions, or exact pass markers to make the scenario pass.
- Run Tasks 6A-6F sequentially. Do not work on multiple stages in parallel because they share one inherited uncommitted diff.
- Use a fresh Terra-medium subagent for each stage. The primary agent reviews scope and correctness before advancing.
- Because inherited Task 6 changes are already interleaved, do not force artificial partial commits. Record each accepted stage in the handoff and make the Task 6 commit only after 6F passes against the complete source revision.

## New-session bootstrap

- [ ] Enter the existing worktree and verify its identity:

  ```sh
  cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
  git branch --show-current
  git log -1 --format='%h %s'
  git status --short --branch
  git diff --check
  git diff --stat
  ```

  Expected branch: `codex/pjsua-plan3`. Expected pushed baseline: `3a22a9588 voip: guard unsupported PJSUA calls`. Expect the modified and untracked files recorded in the handoff; do not reset or delete them.

- [ ] Read, in order:

  ```text
  docs/voip/plan3-session-handoff.md
  docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md
  docs/superpowers/plans/2026-08-27-pjsua-five-account-registration.md
  docs/superpowers/plans/2026-09-05-pjsua-plan3-task6-continuation.md
  designIdead.md
  reviews.md
  state_machine.uml, or docs/superpowers/specs/state_machine.uml if that is the tracked location
  ```

- [ ] Preserve a diagnostic-only copy of the inherited diff outside the repository:

  ```sh
  git diff --binary > /tmp/voip-plan3-task6-inherited.patch
  git status --short > /tmp/voip-plan3-task6-inherited-status.txt
  ```

  These files are recovery aids, not repository artifacts. Do not apply them unless worktree changes are actually lost.

---

## Task 6A: Diagnose and prove nonblocking native event pumping

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify only if the captured backtrace proves it necessary: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`
- Modify only if the captured backtrace proves it necessary: `voip/src/pjsua/PjsuaRuntime.cpp`
- Modify only if the captured backtrace proves it necessary: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Test: `voip/tests/pjsua/PjsuaRuntimeTest.cpp`
- Test: `voip/tests/pjsua/PjsuaRuntimeAdapterTest.cpp`

**Interfaces:**

- Consumes: `PjsuaRuntime::Pump(std::uint32_t timeout_ms)`, `PjsuaRuntimeAdapter::Pump(std::uint64_t now_ms, std::uint32_t timeout_ms)`, `ScriptedRegistrar::Stats()`.
- Produces: a bounded one-account public-API smoke stage proving that a REGISTER reaches `ScriptedRegistrar::OnRequest()`, the actor keeps iterating, and shutdown completes.

- [ ] **Step 1: Reproduce the inherited failure without editing source**

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
    -d /tmp/voip-plan3-registration-6a -- \
    -DEXTRA_CONF_FILE=pjsua_registration.conf

  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  timeout 20s west build -d /tmp/voip-plan3-registration-6a -t run \
    2>&1 | tee /tmp/voip-plan3-registration-6a.log
  ```

  Expected current failure: the TCP connection is accepted, `ScriptedRegistrar::OnRequest()` is not reached, and the actor does not complete later pump turns. If the scenario unexpectedly passes, preserve the log and still add the permanent one-account stage before advancing.

- [ ] **Step 2: Capture the blocking stack before proposing a fix**

  Start the documented QEMU debug runner for the fresh build:

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  west debug -d /tmp/voip-plan3-registration-6a
  ```

  In GDB, break on `voip::PjsuaRuntime::Pump`, continue until the post-bootstrap call, then interrupt after the TCP accept and capture:

  ```gdb
  set pagination off
  break voip::PjsuaRuntime::Pump
  continue
  continue
  thread apply all bt full
  info threads
  frame 0
  info locals
  ```

  Record the exact first blocking PJ/PJSIP function, its caller, and any lock it is waiting for in `docs/voip/plan3-session-handoff.md`. Do not inspect Zephyr implementation source. If the stack cannot be understood without a specific Zephyr source file, stop and request authorization for that file.

- [ ] **Step 3: Add the smallest permanent failing liveness stage**

  Refactor the scenario to reuse one static service:

  ```cpp
  voip::VoipService &ServiceUnderTest() noexcept {
      static voip::VoipService service;
      return service;
  }
  ```

  Add `RunOneAccountSmoke()` with one valid account, no scripted 408, a bounded event wait, and these required assertions:

  ```cpp
  CHECK(service.Initialize(config) == voip::Error::ok);
  CHECK(WaitForRegistered(service, 0, settle_wait_ms));
  CHECK(registrar.Stats().requests != 0);
  CHECK(registrar.Stats().requests == registrar.Stats().tcp_requests);
  CHECK(service.Shutdown() == voip::Error::ok);
  ```

  Print this stage marker only after all assertions pass:

  ```text
  VOIP PJSUA REGISTRATION SMOKE: PASSED (1 account, TCP Digest, actor polling)
  ```

- [ ] **Step 4: Run the smoke stage and confirm it fails for the captured reason**

  Rebuild and rerun `/tmp/voip-plan3-registration-6a`. Expected before the fix: the smoke marker is absent and the captured stack matches Step 2.

- [ ] **Step 5: Implement one causal fix**

  Change only the component identified by the backtrace. Do not combine timeout changes, additional retries, larger stacks, or relaxed assertions. Add a focused fake/runtime regression when the cause can be represented through `PjsuaApi`; the regression must fail without the production fix and pass with it.

- [ ] **Step 6: Remove temporary polling diagnostics and verify the stage**

  Remove all `[PJSUA registration debug]` prints and the `debug_turns` counter. Keep only failure diagnostics, stage markers, stack/TLS qualification output, and final acceptance output.

  Rebuild and rerun the registration profile. Require the exact smoke marker, no hang, and clean shutdown.

- [ ] **Step 7: Run focused component coverage**

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
    -d /tmp/voip-plan3-component-6a -- \
    -DEXTRA_CONF_FILE=pjsua_registration_fake.conf

  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  west build -d /tmp/voip-plan3-component-6a -t run
  ```

  Expected: `PJSUA PLAN 3 COMPONENT RESULT: PASSED`.

- [ ] **Step 8: Review checkpoint**

  The primary agent must verify that the fix follows the captured stack, `Pump(..., 0)` remains actor-owned, no worker thread was enabled, and no unrelated Task 6 behavior was added. Record acceptance in the handoff before Task 6B.

---

## Task 6B: Prove five-account authentication and stable mapping

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`
- Modify if statistics need a public test field: `applications/voip_integration/test_support/ScriptedRegistrar.hpp`

**Interfaces:**

- Consumes: the passing one-account smoke stage, `VoipService::GetAgentHandle()`, public `agent_snapshot` events, and `RegistrarStats`.
- Produces: four independently registered agents, one final 403 authentication failure, configuration-order handles, TCP-only evidence, and untouched audio stubs.

- [ ] **Step 1: Separate basic registration from retry and refresh assertions**

  Add a `RunFiveAccountAuthentication()` stage. Do not arm any 408 in this stage. Configure agents 0-3 with `valid-N` passwords and agent 4 with `wrong-password`.

- [ ] **Step 2: Add the failing public-event assertions**

  Wait for exactly these terminal initial outcomes:

  ```text
  agents 0,1,2,3 -> registered
  agent 4         -> authentication_failed with SIP 403
  ```

  Assert every returned handle's `slot` equals its configuration index. Assert agents 0-3 have no authentication failure and agent 4 receives no retry-wait event.

- [ ] **Step 3: Add isolation and transport assertions**

  Require five distinct mandatory `PcmSource`/`PcmSink` pairs and prove their `Read`, `Write`, and `Flush` methods were never called. Overwrite every caller-owned identity, registrar, username, and password buffer immediately after successful initialization. Require all registrar requests to be TCP and no callback failure.

- [ ] **Step 4: Run the real profile**

  Rebuild and run in `/tmp/voip-plan3-registration-6b`. Print only after all checks pass:

  ```text
  VOIP PJSUA AUTHENTICATION: PASSED (4 registered, 1 rejected, stable handles)
  ```

- [ ] **Step 5: Review checkpoint**

  The primary agent verifies that credentials remain owned by `AgentRegistry`, the account context contains no credential/audio storage, and the stage used only the public service API for observations.

---

## Task 6C: Prove independent 408 retry scheduling

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`
- Test: `voip/tests/pjsua/PjsuaRegistrationStateTest.cpp`
- Test: `voip/tests/pjsua/PjsuaRuntimeAdapterTest.cpp`
- Modify only if a failing test proves it necessary: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Modify only if a failing test proves it necessary: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`

**Interfaces:**

- Consumes: `ScriptedRegistrar::ArmOne408()`, `PjsuaAccountManager::SetNow()`, and actor-provided monotonic milliseconds.
- Produces: isolated 408 transitions and exact bounded retry deadlines using base delays `1, 2, 4, 8, 16, 30, 30` seconds plus `slot * 50 ms` jitter.

- [ ] **Step 1: Re-run the exact fake timing matrix before editing production**

  Require the callback injected at `now_ms == 5000` to schedule agent 2 for `6100`, with no native retry at `6099` and one retry at `6100`. Extend the table to all base delays and all slots. Confirm each failure affects only its target context.

- [ ] **Step 2: Add the real 408 stage**

  In `RunFiveAccountRetryIsolation()`, arm one 408 for agents 1 and 3 before initial registration. Require, independently for each:

  ```text
  transport_failed -> retry_wait -> registering -> registered
  ```

  Require agents 0 and 2 to stay registered and agent 4 to stay authentication-failed. Their retry counters, retry events, and native registration calls must not change.

- [ ] **Step 3: Run component and real profiles**

  Rebuild/run `pjsua_registration_fake.conf`, then rebuild/run `pjsua_registration.conf` in fresh `-6c` build directories. Require:

  ```text
  PJSUA PLAN 3 COMPONENT RESULT: PASSED
  VOIP PJSUA RETRY: PASSED (agents 1 and 3 isolated)
  ```

- [ ] **Step 4: Review checkpoint**

  Verify callbacks receive the current actor time before they schedule work, PJSUA native retry remains disabled, no sleeping occurs in account logic, and retries are actor-pumped.

---

## Task 6D: Prove refresh isolation

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`
- Test if behavior changes: `voip/tests/pjsua/PjsuaRegistrationStateTest.cpp`

**Interfaces:**

- Consumes: short registrar expiration, copied PJSUA registration callbacks, and independent account timers.
- Produces: observable `refreshing -> registered` cycles for at least agents 0 and 2 without changing any other agent's retry state.

- [ ] **Step 1: Add bounded refresh assertions**

  Respond to successful registration with the existing short expiration. Require agents 0 and 2 to emit `refreshing`, then a second `registered`, within the scenario deadline.

- [ ] **Step 2: Assert isolation**

  Snapshot retry event counts before the refresh window. After both refreshes, require unchanged retry counts for agents 0, 2, and 4 and no new false failure for any agent.

- [ ] **Step 3: Run the real profile**

  Rebuild/run in `/tmp/voip-plan3-registration-6d`. Require:

  ```text
  VOIP PJSUA REFRESH: PASSED (agents 0 and 2 independent)
  ```

- [ ] **Step 4: Review checkpoint**

  Verify the registrar did not simulate refresh by directly changing service state and that all observations came from native PJSUA callbacks copied through the router.

---

## Task 6E: Prove one complete shutdown and zeroization lifecycle

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify: `applications/voip_integration/src/pjsua_arena_test.c`
- Modify if a failing test proves it necessary: `pjproject/zephyr/include/pj_zephyr_pool_arena.h`
- Modify if a failing test proves it necessary: `pjproject/zephyr/pj_zephyr_pool_arena.c`
- Test: `voip/tests/unit/AgentRegistryTest.cpp`
- Test: `voip/tests/pjsua/PjsuaPublicServiceTest.cpp`
- Test: `voip/tests/pjsua/PjsuaRuntimeTest.cpp`
- Test: `voip/tests/pjsua/PjsuaRuntimeAdapterTest.cpp`

**Interfaces:**

- Consumes: idempotent actor-driven shutdown, `CredentialsErasedForTest()`, callback-router test state, public handles, resource snapshot, and private arena statistics.
- Produces: one shutdown while a retry is pending, with `service_stopped` final and all native/public/secret state cleaned safely.

- [ ] **Step 1: Finish focused fake failure coverage**

  Require startup cleanup to retry PJ thread registration and native destroy failures without releasing callback-owned storage. Require `pjsua_destroy()` success before arena reset and router detach. Require a later shutdown turn to recover when the injected failure is cleared.

- [ ] **Step 2: Validate arena reset outside the spinlock**

  In `pjsua_arena_test.c`, allocate enough blocks to make `arena_high_water` nonzero, reset, and require:

  ```c
  stats.live_blocks == 0
  stats.used_bytes == 0
  stats.last_reset_wiped_bytes > 0
  stats.last_reset_wiped_bytes <= stats.capacity_bytes
  stats.reset_in_progress == 0
  ```

  Review that allocation and install reject `RESETTING`, volatile erasure occurs with interrupts enabled, and metadata becomes `READY` only after zeroization completes.

- [ ] **Step 3: Shut down the real service with a retry pending**

  Arm another one-shot 408 for agent 1, wait for `retry_wait`, call `Shutdown()`, then drain the public queue. Require one `service_stopped` event and no event after it.

- [ ] **Step 4: Assert complete post-shutdown state**

  Require:

  ```text
  pjsua_acc_get_count() == 0
  pjsua_get_state() == PJSUA_STATE_NULL
  PjsuaCallbackRouter::ActiveForTest() == nullptr
  arena live_blocks == 0
  arena used_bytes == 0
  arena last_reset_wiped_bytes > 0
  every previous AgentHandle is invalid
  ResourceSnapshot active_agents == 0
  ResourceSnapshot active_calls == 0
  focused AgentRegistry reset/failure tests report credential storage erased
  TLS probe passed
  actor minimum unused stack is nonzero
  ```

- [ ] **Step 5: Run focused and real profiles**

  Run the arena profile, Plan 3 fake component profile, and real registration profile in fresh `-6e` directories. Require their exact pass markers plus:

  ```text
  VOIP PJSUA LIFECYCLE: PASSED (pending retry, ordered teardown, zeroized state)
  ```

- [ ] **Step 6: Review checkpoint**

  Independently review destructor lifetime, startup-cleanup retry, arena reset locking, and credential erasure. Reject any bounded retry count that permits PJSUA-owned callbacks to outlive their storage.

---

## Task 6F: Repeat five complete lifecycles and accept Task 6

**Files:**

- Modify: `applications/voip_integration/src/pjsua_registration.cpp`
- Modify only for final cleanup: Task 6 production and test files
- Update: `docs/voip/plan3-session-handoff.md`

**Interfaces:**

- Consumes: every accepted stage from Tasks 6A-6E.
- Produces: the exact original Task 6 terminator after five complete lifecycles against one stable source revision.

- [ ] **Step 1: Compose the full lifecycle without removing stage assertions**

  Run authentication, isolated 408 recovery, refresh, pending-retry shutdown, and teardown checks in every lifecycle. Reuse one static `VoipService`; reset only registrar test state between lifecycles.

- [ ] **Step 2: Repeat exactly five times**

  Require all five lifecycle counters and arena reset-wipe counters to advance monotonically. A failure in any lifecycle suppresses the final pass marker.

- [ ] **Step 3: Remove temporary diagnostics and run the final real profile**

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
    -d /tmp/voip-plan3-registration-6f -- \
    -DEXTRA_CONF_FILE=pjsua_registration.conf

  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  west build -d /tmp/voip-plan3-registration-6f -t run
  ```

  Required exact terminator:

  ```text
  VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)
  ```

- [ ] **Step 4: Run the complete fake component profile**

  Require `PJSUA PLAN 3 COMPONENT RESULT: PASSED` from a fresh build using `pjsua_registration_fake.conf`.

- [ ] **Step 5: Run source checks and independent Task 6 review**

  ```sh
  cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
  rg -n '\[PJSUA registration debug\]|debug_turns' applications/voip_integration voip || true
  git diff --check
  git status --short
  ```

  The debug search must return no matches. Review the complete Task 6 diff against the architecture, `designIdead.md`, `reviews.md`, and the state machine. Confirm no call-state or FIFO behavior changed.

- [ ] **Step 6: Commit Task 6 after review passes**

  ```sh
  git add applications/voip_integration pjproject/zephyr voip \
    docs/voip/plan3-session-handoff.md \
    docs/superpowers/plans/2026-09-05-pjsua-plan3-task6-continuation.md
  git diff --cached --check
  git commit -m "test(voip): validate five pjsua registrations"
  ```

---

## Task 7: Run the reduced regression gate and record acceptance

**Files:**

- Create: `docs/voip/pjsua-five-account-registration.md`
- Modify only if a failing gate proves it necessary: Plan 3 production/test files

**Interfaces:**

- Consumes: the committed Task 6 source revision and existing Plan 1/Plan 2 regression profiles.
- Produces: one acceptance record with exact commands, pass markers, memory evidence, limitations, and design-review conclusions.

- [ ] **Step 1: Run the host suite twice**

  ```sh
  cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
  bash voip/tests/run_host_tests.sh
  bash voip/tests/run_host_tests.sh
  ```

  Both runs must pass every test, including no-heap, shutdown, credentials, call state machine, scheduler, and FIFO coverage.

- [ ] **Step 2: Run both Plan 3 profiles from clean build directories**

  Rebuild/run `pjsua_registration_fake.conf` and `pjsua_registration.conf`. Capture exact pass markers, zero worker-thread evidence, arena peak/reset values, actor minimum unused stack, and five-lifecycle output.

- [ ] **Step 3: Run the Plan 1 capacity regression**

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration \
    -d /tmp/voip-plan3-capacity-task7 -- \
    -DEXTRA_CONF_FILE=pjsua_capacity.conf

  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  west build -d /tmp/voip-plan3-capacity-task7 -t run
  ```

  Require: `PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)`.

- [ ] **Step 4: Verify security and heap boundaries**

  Run negative public-service coverage requiring TLS and SRTP requests to return `unsupported_configuration` before router attach, arena install, native create, or account creation. Run the no-heap scenario across initialize, register, refresh, 408, retry, polling, and shutdown; any general-heap allocation after successful initialization is a release blocker.

- [ ] **Step 5: Review architectural exclusions**

  Confirm the diff contains no call manager, media bridge, codec-priority policy, TLS implementation, SRTP implementation, PJSUA2 use, runtime account mutation, legacy-stack deletion, call-state transition change, or FIFO-policy change.

- [ ] **Step 6: Write the acceptance record**

  In `docs/voip/pjsua-five-account-registration.md`, record exact commands and observed output from this source revision. State explicitly that RT1064 compile/hardware/network-interface qualification was removed from this continuation and remains future work; do not imply embedded acceptance.

- [ ] **Step 7: Verify and commit Task 7**

  If any gate fails, stop, diagnose it as a regression in the task that owns the behavior, add a focused failing test, apply one causal fix, and restart Task 7 from Step 1. Commit reviewed code fixes separately before committing the acceptance record.

  ```sh
  cd /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3
  git diff --check
  git status --short
  git add docs/voip/pjsua-five-account-registration.md
  git diff --cached --check
  git commit -m "docs(voip): record five-account registration acceptance"
  ```

## Continuation exit criteria

- Task 6A has a captured root cause and a permanent passing one-account liveness smoke test.
- Tasks 6B-6E each pass independently before the next behavior is added.
- Task 6F prints the exact five-account/five-lifecycle terminator with no temporary diagnostics.
- The host suite passes twice, the fake component profile passes, and Plan 1 capacity remains unchanged.
- Credentials and reachable arena memory are erased after shutdown without long interrupt-disabled wiping.
- PJSUA/PJMEDIA worker counts remain zero, SIP remains TCP-only, and TLS/SRTP remain disabled.
- The call state machine, per-agent one-call limit, two-global-call limit, and five-entry FIFO are unchanged.
- The acceptance record identifies RT1064 qualification as deferred.
- There is no Task 8 in this continuation.
