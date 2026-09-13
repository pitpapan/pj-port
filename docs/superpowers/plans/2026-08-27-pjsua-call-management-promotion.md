# PJSUA Call Management and Two-Slot Promotion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect the completed five-account PJSUA service to incoming and outgoing calls while preserving seven logical calls, two global promoted leases, one promoted call per agent, and the strict shared five-entry FIFO.

**Architecture:** `VoipRuntime`, `CallScheduler`, and `CallStateMachine` remain the only owners of product admission, FIFO order, public state, handles, operations, and guaranteed events. A new actor-owned `PjsuaCallManager` owns only native PJSUA call records and translates between opaque runtime tokens and `pjsua_call_id`; `PjsuaRuntimeAdapter` composes it with the Plan 3 runtime, account, callback-router, and transport components. Incoming admission is a two-phase transaction so no `180 Ringing` is sent until logical capacity and both guaranteed events are reserved.

**Tech Stack:** C++17, PJSUA-LIB C API, PJSIP INVITE/SDP over SIP TCP, plain RTP/RTCP negotiation, fixed arrays, generation-protected handles, fake PJSUA component tests, scripted loopback SIP peers, Zephyr QEMU, and a compile-only `mimxrt1064_evk` gate.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Business state machine:** `docs/superpowers/specs/state_machine.uml`

**Plan 3 baseline:** `987076845` records acceptance of source revision `c6a9a97b9`. Read `designIdead.md`, `reviews.md`, and `docs/voip/pjsua-five-account-registration.md` before changing code.

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`; use documented `west` commands and build output.
- Preserve one to five immutable agents initialized once, one PJSUA account per agent, and exactly one process-global initialized PJSUA service.
- Preserve fixed capacities: five agents, seven logical/native call contexts, two promoted leases, five FIFO entries, 16 commands, 16 operations, and 32 events.
- `CallScheduler` already owns admission, strict FIFO, head-of-line blocking, one-promoted-call-per-agent, and logical handle generations. Plan 4 consumes and narrowly extends it; it does not create a second scheduler.
- `CallStateMachine` remains the only mutable owner of public `CallState` and `HoldReason`. Native phases never duplicate or bypass `state_machine.uml`.
- PJSUA-LIB owns SIP dialog, INVITE, SDP, hold, and native teardown. Product code owns capacity, FIFO order, handles, timers, and event guarantees.
- Only the VoIP actor invokes mutable PJSUA APIs. PJSUA/PJMEDIA worker counts remain zero; the actor drives `pjsua_handle_events()`.
- Callbacks synchronously copy bounded values. Never retain `pjsip_rx_data *`, `pjsua_call_info` string views, SDP pool pointers, callback-local addresses, or application callbacks.
- A queued outgoing call owns a logical handle and copied URI but no PJSUA ID and sends no INVITE. A queued incoming call retains its early PJSUA ID and receives `180 Ringing` only after transactional admission.
- Promotion alone does not mean acceptance. A call in `hold(waiting)` stays there until PJSUA confirms acceptance.
- Two eligible FIFO entries may promote in one capacity pass when two leases are free. FIFO controls order; it does not serialize otherwise eligible promotions.
- Queue timing starts at admission. Answer/establishment timing starts or restarts at promotion.
- Commit one terminal event before handle invalidation. Clear native user data and release a promoted lease only after signaling, media state, and callback entry are quiescent.
- No C++ or general system-heap allocation is allowed after initialization. PJPROJECT uses only the fixed VoIP arena.
- SIP uses plain TCP and media negotiation uses plain RTP/RTCP. TLS and SRTP stay disabled and return `unsupported_configuration` before native call allocation.
- Codec policy, custom audio attachment, and the two media bridges remain Plan 5 scope. Plan 4 retains the PJSUA no-sound-device boundary and exposes a future media hook.
- Do not remove the legacy uppercase stack.
- QEMU is not embedded acceptance. Plan 4 requires RT1064 compile/link and memory reports only; hardware audio, network selection, and runtime qualification remain later work.

---

### Task 1: Add the bounded PJSUA call API seam and component profile

**Files:**

- Modify: `voip/src/pjsua/PjsuaApi.hpp`
- Modify: `voip/src/pjsua/PjsuaApi.cpp`
- Modify: `voip/tests/pjsua/FakePjsuaApi.hpp`
- Create: `voip/tests/pjsua/PjsuaCallApiTest.cpp`
- Create: `voip/tests/pjsua/PjsuaPlan4TestMain.cpp`
- Create: `applications/voip_integration/pjsua_call_component.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: Plan 3’s function table and component-test injection seam.
- Produces: fakeable PJSUA call primitives without exposing PJPROJECT publicly.

- [ ] Write `RunPjsuaCallApiTests()` first. Require every new entry to be non-null and assert this fake sequence:

  ```cpp
  assert(fake.SequenceEquals(
      "call-defaults,make-call,set-call-data,get-call-data,get-call-info,"
      "answer-180,answer-200,hold,reinvite,hangup"));
  ```

- [ ] Build the new QEMU component profile and confirm compilation fails because the entries are absent:

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/applications/voip_integration \
    -d /tmp/voip-plan4-component-task1 -- \
    -DEXTRA_CONF_FILE=pjsua_call_component.conf
  ```

- [ ] Add and natively wire these private `PjsuaApi` entries:

  ```cpp
  void (*call_setting_default)(pjsua_call_setting *) = nullptr;
  pj_status_t (*call_make_call)(pjsua_acc_id, const pj_str_t *,
      const pjsua_call_setting *, void *, const pjsua_msg_data *,
      pjsua_call_id *) = nullptr;
  pj_status_t (*call_set_user_data)(pjsua_call_id, void *) = nullptr;
  void *(*call_get_user_data)(pjsua_call_id) = nullptr;
  pj_status_t (*call_get_info)(pjsua_call_id, pjsua_call_info *) = nullptr;
  pj_status_t (*call_answer2)(pjsua_call_id, const pjsua_call_setting *,
      unsigned, const pj_str_t *, const pjsua_msg_data *) = nullptr;
  pj_status_t (*call_set_hold2)(pjsua_call_id, unsigned,
      const pjsua_msg_data *) = nullptr;
  pj_status_t (*call_reinvite2)(pjsua_call_id,
      const pjsua_call_setting *, const pjsua_msg_data *) = nullptr;
  ```

  Retain existing `call_answer` and `call_hangup` entries used by Plan 3 guards.

- [ ] Implement at most seven fixed fake call records. Reject the eighth with `PJ_ETOOMANY`, validate IDs, support per-entry failure injection, and zero user data on `Reset()`.

- [ ] Run the profile and require `PjsuaCallApiTest PASSED`, harness-owned timeout cleanup, and no fault or abort signature.

- [ ] Run `voip/tests/run_host_tests.sh` plus the Plan 3 fake component regression.

- [ ] Commit:

  ```sh
  git add applications/voip_integration voip/src/pjsua/PjsuaApi.* \
    voip/tests/pjsua/FakePjsuaApi.hpp voip/tests/pjsua/PjsuaCallApiTest.cpp \
    voip/tests/pjsua/PjsuaPlan4TestMain.cpp
  git commit -m "test(voip): add bounded pjsua call api seam"
  ```

### Task 2: Add native call contexts and callback-safe routing

**Files:**

- Create: `voip/src/pjsua/PjsuaCallRecords.hpp`
- Create: `voip/src/pjsua/PjsuaCallContext.hpp`
- Create: `voip/src/pjsua/PjsuaCallManager.hpp`
- Create: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaCallbackRouter.hpp`
- Modify: `voip/src/pjsua/PjsuaCallbackRouter.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.hpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Create: `voip/tests/pjsua/PjsuaCallContextTest.cpp`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: `PjsuaAccountManager::Resolve(pjsua_acc_id)`, `NativeId(AgentHandle, pjsua_acc_id *)`, call callbacks, and Task 1’s API table.
- Produces: seven stable native contexts, opaque nonzero `std::uint32_t` tokens, and copied `RuntimeNotification` records.

- [ ] Write RED tests for seven non-slot-order native IDs, eighth exhaustion, duplicate IDs, token reuse, user-data mismatch, late callback after release, notification pressure, and reset.

- [ ] Define native ownership without a second business-state store:

  ```cpp
  enum class PjsuaCallPhase : std::uint8_t {
      free, provisional_incoming, queued_incoming, promoting,
      calling, early, confirmed, local_hold, disconnecting, disconnected
  };
  struct PjsuaCallContext {
      std::uint32_t token = 0;
      CallHandle logical{};
      AgentHandle agent{};
      pjsua_call_id native_id = PJSUA_INVALID_ID;
      PjsuaCallPhase phase = PjsuaCallPhase::free;
      bool incoming = false, logical_bound = false, user_data_bound = false;
      bool media_quiescent = true, terminal_reported = false;
      unsigned callback_depth = 0;
  };
  ```

  Do not add `CallState`, `HoldReason`, FIFO position, operation ownership, audio pointers, or scheduler policy.

- [ ] Add `PjsuaCallCallbackSink` with synchronous `OnIncomingCall(account,id)`, `OnCallState(id)`, and `OnCallMediaState(id)`. Install the three PJSUA callbacks in `PjsuaCallbackRouter`; discard `pjsip_rx_data *` before return.

- [ ] On incoming callback, resolve account→agent, reserve a native context, bind user data, synchronously copy `call_get_info`, and enqueue one incoming notification. Unknown account gets 404; context/notification exhaustion gets 486 and complete native cleanup.

- [ ] Compose `PjsuaCallManager calls_` inside `PjsuaRuntimeAdapter` and drain its copied ring after every `pjsua_handle_events()`. Keep controls unsupported until their tasks land.

- [ ] Run focused, Plan 3 component, and host tests; require exact pre-test resource baselines after every case.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua/PjsuaCallContextTest.cpp \
    applications/voip_integration/CMakeLists.txt
  git commit -m "feat(voip): add bounded pjsua call contexts"
  ```

### Task 3: Make incoming admission transactional

**Files:**

- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.cpp`
- Modify: `voip/src/core/CallScheduler.hpp`
- Modify: `voip/src/core/CallScheduler.cpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.hpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Create: `voip/tests/pjsua/PjsuaIncomingAdmissionTest.cpp`
- Modify: `voip/tests/unit/CallSchedulerTest.cpp`
- Modify: `voip/tests/unit/VoipServiceCoreTest.cpp`

**Interfaces:**

- Consumes: a provisional incoming token, guaranteed incoming/terminal reservations, and `CallScheduler::AdmitIncoming()`.
- Produces: a bound logical/native context and committed 180 only after admission succeeds.

- [ ] Write RED cases for unknown account→404, every capacity/reservation failure→486, immediate/queued success→180, 180 failure→rollback, and remote CANCEL after admission.

- [ ] Extend every private adapter implementation:

  ```cpp
  virtual Error AdmitIncoming(CallHandle, AgentHandle,
                              std::uint32_t token) noexcept = 0;
  virtual Error PromoteIncoming(CallHandle, AgentHandle,
                                std::uint32_t token) noexcept = 0;
  ```

  Add `RuntimeRequest::admit_incoming` and `RuntimeRequest::call`. Fake admission records requests but emits no acceptance callback.

- [ ] Add actor-only rollback for a handle never exposed publicly:

  ```cpp
  Error RollbackUnpublishedAdmission(CallHandle,
                                     SchedulerEffects &) noexcept;
  ```

  It removes FIFO/lease/context without publishing a business transition and restores exact resource counts.

- [ ] Implement this `VoipRuntime` order:

  ```text
  reserve incoming event -> reserve terminal event -> scheduler admission
  -> adapter.AdmitIncoming(handle, agent, token) and send 180
  -> commit incoming event -> publish state -> apply promotion effects
  ```

  Any pre-commit failure cancels both reservations, rolls back the logical admission, and rejects/drains the provisional native call without exposing a handle.

- [ ] In `PjsuaCallManager`, validate token/user-data/agent, bind the logical handle, send `call_answer2(...,180,...)`, then mark admitted. A failed 180 retains callback-reachable state until native disconnection.

- [ ] Prove queued admission has no promotion request and no promoted lease; immediate admission emits one ordered promotion request after 180.

- [ ] Run focused component and complete host tests.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/src/pjsua voip/tests/unit \
    voip/tests/pjsua/PjsuaIncomingAdmissionTest.cpp
  git commit -m "feat(voip): admit incoming pjsua calls transactionally"
  ```

### Task 4: Promote outgoing calls only when eligible

**Files:**

- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.cpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Create: `voip/tests/pjsua/PjsuaOutgoingPromotionTest.cpp`
- Modify: `voip/tests/unit/VoipServiceCoreTest.cpp`

**Interfaces:**

- Consumes: ordered `SchedulerEffects`, copied URI, registered-agent mapping, and a reserved terminal event.
- Produces: `PromoteOutgoing(CallHandle, AgentHandle, const char *, std::uint32_t *)` and one native call only at promotion.

- [ ] Write RED cases: unregistered agent has no operation ID; immediate dial makes one call; queued dial makes none; pre-promotion cancel makes none; later promotion uses the original copied URI; seventh native call succeeds and eighth fails cleanly.

- [ ] Carry the logical handle through every adapter:

  ```cpp
  virtual Error PromoteOutgoing(CallHandle, AgentHandle, const char *,
                                std::uint32_t *token) noexcept = 0;
  ```

- [ ] Resolve the agent account, reserve a native context, copy URI, default `pjsua_call_setting`, set `aud_cnt=1` and `vid_cnt=0`, call `call_make_call`, bind native ID/user data, and return the opaque token. Map native capacity to `resource_exhausted`; map signaling/SDP errors without native strings.

- [ ] Correct operation semantics: queued Dial completes `ok` at durable FIFO admission; immediate Dial completes when `call_make_call` accepts it. PJSUA acceptance/rejection is a later call event. Remove the current pending `signaling_operation` behavior for Dial.

- [ ] Process up to two promotion effects in array order. Failure of the first publishes/finalizes its terminal event, then processing continues with the second without FIFO reordering.

- [ ] Run outgoing, operation, scheduler, and host tests.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/src/pjsua \
    voip/tests/pjsua/PjsuaOutgoingPromotionTest.cpp \
    voip/tests/unit/VoipServiceCoreTest.cpp
  git commit -m "feat(voip): promote outgoing calls through pjsua"
  ```

### Task 5: Implement controls and promotion-relative timers

**Files:**

- Modify: `voip/src/core/CallContext.hpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Create: `voip/tests/pjsua/PjsuaCallControlTest.cpp`
- Modify: `voip/tests/unit/CallSchedulerTest.cpp`
- Modify: `voip/tests/unit/VoipServiceCoreTest.cpp`

**Interfaces:**

- Consumes: actor-owned controls, native tokens, queue/answer durations, and PJSUA control APIs.
- Produces: one pending control per call and independent timer epochs.

- [ ] Write a RED command/state matrix for queued outgoing/incoming, promoted incoming, outgoing/early, established, `hold(media)`, disconnecting, and stale handles. A second command while native negotiation is pending completes `busy` rather than cancelling the first operation.

- [ ] Replace ambiguous `admitted_at_ms` timing with:

  ```cpp
  std::uint64_t queue_started_at_ms = 0;
  std::uint64_t answer_started_at_ms = 0;
  bool queue_timer_active = false;
  bool answer_timer_active = false;
  ```

  Queue timing starts at FIFO insertion and stops at promotion. Answer/establishment timing starts on immediate promotion or restarts when a queued call is promoted. Use subtraction-based elapsed checks.

- [ ] Implement controls exactly: queued incoming Answer sets `answer_on_promotion` and sends no 200; promoted Answer sends 200; queued incoming Reject sends validated 400–699; queued outgoing Cancel sends no SIP; outgoing early Cancel hangs up compatibly with 487; established/held Hangup calls PJSUA; hold uses `call_set_hold2`; resume uses `call_reinvite2` with unhold settings.

- [ ] Complete answer/reject/cancel/hangup operations when local mutation or PJSUA accepts the request. Hold/resume stays pending until copied media confirmation or terminal failure. Every accepted operation completes once.

- [ ] Queue timeout sends 480 for incoming and no SIP for outgoing. Promoted establishment/answer timeout begins native teardown. Both commit the terminal event before logical invalidation.

- [ ] Prove a call queued for `answer_timeout_ms-1` receives a full answer window after promotion, and prove `UINT64_MAX`-adjacent elapsed checks do not fire early.

- [ ] Run focused, Plan 3, and host regressions.

- [ ] Commit:

  ```sh
  git add voip/src/core/CallContext.hpp voip/src/core/VoipRuntime.cpp \
    voip/src/pjsua voip/tests/pjsua/PjsuaCallControlTest.cpp \
    voip/tests/unit/CallSchedulerTest.cpp voip/tests/unit/VoipServiceCoreTest.cpp
  git commit -m "feat(voip): add pjsua call controls and timers"
  ```

### Task 6: Translate native state and drain calls safely

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallRecords.hpp`
- Modify: `voip/src/pjsua/PjsuaCallContext.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Create: `voip/tests/pjsua/PjsuaCallStateTest.cpp`
- Modify: `voip/tests/pjsua/PjsuaRuntimeAdapterTest.cpp`

**Interfaces:**

- Consumes: call-state/media callbacks, copied `pjsua_call_info`, pending operations, and `BeginCallTeardown(token)`.
- Produces: semantic runtime notifications and one final `call_finished` only after native ownership is unreachable.

- [ ] Write RED sequences for CALLING→EARLY→CONFIRMED→DISCONNECTED, incoming confirmation, 3xx–6xx rejection, remote CANCEL, transport/SDP failures, hold/resume success/failure, duplicates, ring pressure, late callbacks, and shutdown with calls.

- [ ] In each callback, call `call_get_info` synchronously and copy only token, agent, semantic type, SIP status, mapped error, bounded remote URI/address, media activity, and sanitized reason. No PJ pointer or string view enters a ring.

- [ ] Emit semantic causes only: accepted on confirmation; rejected on final pre-establishment response; held/resumed on confirmed media direction; timeout only from product timer. Never store a second public `CallState`.

- [ ] Make terminal handling idempotent. First terminal classification wins, each pending operation completes once, and remote CANCEL overrides queued answer-on-promotion.

- [ ] Release only when signaling is disconnected, media is quiescent, `callback_depth==0`, and future security cleanup is clear. Then clear user data, invalidate native context/token, enqueue one `call_finished`, and let core commit terminal event before logical release/promotion.

- [ ] Hold a media callback open while DISCONNECTED arrives. The lease must remain held. After quiescence require one terminal event, immediately invalid old handle, pollable copied snapshot, and only then FIFO promotion.

- [ ] Prove shutdown order: stop admission/promotion; cancel queued outgoing; reject queued incoming; drain promoted calls; clear call user data; unregister/delete accounts; close transport; destroy PJSUA; wipe arena/credentials; publish `service_stopped`; stop actor. Timeout retains callback-reachable storage.

- [ ] Run component, host, registration, and capacity regressions.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/src/core/RuntimeAdapter.hpp \
    voip/src/core/VoipRuntime.cpp voip/tests/pjsua
  git commit -m "feat(voip): translate and drain pjsua call state"
  ```

### Task 7A: Prove two real calls with a scripted SIP peer

**Files:**

- Create: `applications/voip_integration/test_support/ScriptedSipPeer.hpp`
- Create: `applications/voip_integration/test_support/ScriptedSipPeer.cpp`
- Create: `applications/voip_integration/src/multi_call_scheduler.cpp`
- Create: `applications/voip_integration/multi_call_scheduler.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: five registrations, public polling API, loopback SIP TCP, and plain SDP/RTP negotiation.
- Produces: bounded peer scripts and a two-promoted-call smoke test.

- [ ] Write a failing scenario: outgoing A plus incoming B, answer B, require two valid calls, `promoted_calls==2`, distinct agent leases, copied remote identities, and zero workers.

- [ ] Build `ScriptedSipPeer` as a separate fixed-buffer TCP peer. Parse bounded Call-ID/CSeq/tags and generate INVITE/100/180/200/ACK/BYE/CANCEL/re-INVITE flows without product internals or native endpoint pointers.

- [ ] Prove malformed SDP→`negotiation_failed`, remote 486→`remote_rejected`, transport close→`signaling_failed`, remote CANCEL→`cancelled`, and BYE→one terminal event. Both leases must remain reusable.

- [ ] Build/run:

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DISABLE=1 \
  west build -p always -b mps2/an385 \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/applications/voip_integration \
    -d /tmp/voip-plan4-two-calls -- \
    -DEXTRA_CONF_FILE=multi_call_scheduler.conf
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
  timeout 60s west build -d /tmp/voip-plan4-two-calls -t run
  ```

  Require `VOIP PJSUA TWO CALL SMOKE: PASSED (2 promoted, 2 agents)`.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): add two-call pjsua sip peer"
  ```

### Task 7B: Prove FIFO, controls, capacity, and repeated cleanup

**Files:**

- Modify: `applications/voip_integration/src/multi_call_scheduler.cpp`
- Modify: `applications/voip_integration/test_support/ScriptedSipPeer.hpp`
- Modify: `applications/voip_integration/test_support/ScriptedSipPeer.cpp`
- Modify: `applications/voip_integration/test_support/GeneralHeapTracker.cpp`

**Interfaces:**

- Consumes: Task 7A’s peer and complete public call API.
- Produces: the real `2 promoted + 5 FIFO` matrix over five lifecycles.

- [ ] Establish promoted A/B. Enqueue incoming A, outgoing C, incoming D, outgoing E, incoming B. Queued incoming gets 180/native ID; outgoing C/E sends no INVITE.

- [ ] End B while A stays promoted: the free slot remains idle because incoming A is FIFO head and A is busy. End A: incoming A promotes first and outgoing C may promote second in the same capacity pass; peer order must show A’s action before C’s INVITE.

- [ ] Fill two promoted plus five queued incoming calls; eighth incoming gets 486 with no leak. Separately expire queued incoming→480. Fill FIFO logically and require outgoing Dial→`queue_full` with no native call.

- [ ] Exercise queued answer/reject/cancel, remote CANCEL, both promoted timeouts, hangup/BYE, hold, and resume. Verify exact UML traces.

- [ ] Track general heap immediately after initialization through calls/shutdown and require zero allocations. Record arena values; normal peak≤75%, injected failure peak<90%, no fallback.

- [ ] Repeat five lifecycles using one static service. After each require zero native/logical calls, operations, reservations, accounts, transport/router, credentials, and arena ownership; old handles are invalid.

- [ ] Require `VOIP MULTI CALL RESULT: PASSED (2 promoted, 5 FIFO, 5 lifecycles)`.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): validate pjsua call promotion and fifo"
  ```

### Task 7C: Run final gates, compile RT1064, and record acceptance

**Files:**

- Create: `docs/voip/pjsua-call-management-promotion.md`
- Modify only after causal RED evidence and independent review: the exact failing component files.

**Interfaces:**

- Consumes: stable reviewed Task 1–7B source.
- Produces: reproducible Plan 4 acceptance and a merge-ready branch.

- [ ] Run `voip/tests/run_host_tests.sh` twice; require all Plan 1–4 markers.

- [ ] Fresh-build/run `pjsua_registration_fake.conf`, `pjsua_registration.conf`, and `pjsua_capacity.conf`; require exact Plan 3 component, five-account/five-lifecycle/general-heap, and Plan 1 capacity markers without application fault.

- [ ] Fresh-build/run `pjsua_call_component.conf` and `multi_call_scheduler.conf`; require focused markers, clean harness cleanup, two-call smoke, final multi-call marker, zero heap, arena limits, zero workers, and complete teardown.

- [ ] Compile and report RT1064 without selecting a network driver:

  ```sh
  cd /home/pitpapan/zephyrproject
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DISABLE=1 \
  west build -p always -b mimxrt1064_evk \
    -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/applications/voip_integration \
    -d /tmp/voip-plan4-rt1064 -- \
    -DEXTRA_CONF_FILE=pjsua_call_component.conf
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH west build -d /tmp/voip-plan4-rt1064 -t ram_report
  PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH west build -d /tmp/voip-plan4-rt1064 -t rom_report
  ```

  If it cannot fit configured memory, reconsider fixed storage; do not enlarge target RAM or add heap fallback. Do not claim hardware/network/audio runtime acceptance.

- [ ] Audit the diff: no media bridge/device audio, codec policy, TLS/SRTP, PJSUA2, runtime agent mutation, scheduler bypass, second business-state table, Zephyr-source change, or legacy deletion.

- [ ] Write the acceptance record with revision, commands/statuses, markers, flash/RAM, actor stack, arena/heap evidence, limitations, and QEMU-versus-RT1064 distinction.

- [ ] Obtain independent full-diff review against the spec, `designIdead.md`, `reviews.md`, UML, Plan 3 acceptance, and this plan. Every Critical/Important finding gets focused RED/GREEN repair, scoped re-review, and affected-gate restart.

- [ ] Commit:

  ```sh
  git add docs/voip/pjsua-call-management-promotion.md
  git diff --cached --check
  git commit -m "docs(voip): record plan4 call acceptance"
  ```

## Plan 4 Exit Criteria

- Incoming calls map account→agent and receive visible transactional admission or final rejection.
- Queued incoming retains native state/180; queued outgoing has no native call/INVITE.
- Core UML state machine remains the sole public-state authority; PJSUA reports causes only.
- Exactly two calls promote globally and no agent owns more than one.
- Strict FIFO, head blocking, and ordered promotion of up to two eligible entries pass at core, fake, and SIP levels.
- Queue timeout is admission-relative; answer/establishment timeout is promotion-relative.
- Controls, failures, and operations complete exactly once.
- One terminal event precedes immediate handle invalidation; its copied snapshot remains pollable.
- Native user data/context/lease survive until signaling, media, callbacks, and future security cleanup are quiescent.
- Five complete `2 promoted + 5 FIFO` lifecycles leave zero logical/native calls, operations, reservations, accounts, transport, router, credentials, or arena ownership.
- General heap stays unused after initialization and arena use meets 75%/90% limits.
- Plan 1–3 regressions stay green; RT1064 compile/link and memory reports complete without a hardware-runtime claim.
- Plan 5 receives a stable promoted-call media hook; Plan 4 contains no custom audio bridge.
