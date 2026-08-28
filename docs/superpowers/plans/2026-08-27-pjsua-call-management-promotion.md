# PJSUA Call Management and Two-Slot Promotion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate incoming and outgoing PJSUA calls with seven logical contexts, two global promoted slots, at most one promoted call per agent, and the approved strict shared FIFO.

**Architecture:** `PjsuaCallManager` translates PJSUA callbacks and call-control commands but delegates admission/promotion order to `CallScheduler`. Queued outgoing calls have no PJSUA ID. Queued incoming calls retain their early PJSUA ID and receive 180. A stable seven-record `PjsuaCallContext` table maps optional native IDs to logical handles; native resources and promoted leases are released only after full teardown.

**Tech Stack:** C++17, PJSUA-LIB call APIs, PJSIP INVITE/SDP, fixed arrays, fake PJSUA API tests, scripted SIP peers over TCP, public polling API.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Business state machine:** `docs/superpowers/specs/state_machine.uml`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- PJSUA-LIB owns SIP dialog, INVITE, SDP, hold, and teardown mechanics.
- Product code owns capacity, FIFO, per-agent exclusion, logical handles, and timers.
- Keep exactly seven logical/PJSUA contexts, two promoted leases, and five FIFO entries.
- Queued outgoing calls must not send INVITE or allocate a PJSUA call ID.
- Queued incoming calls receive 180 and retain a PJSUA call ID.
- Full admission/PJSUA call-ID capacity rejects incoming with 486; queue timeout uses 480.
- Custom agent audio attachment is deferred to Plan 5; call/media state must expose a clean hook.
- Keep the five public business states separate from private scheduler/PJSUA
  phases. `hold(waiting)` is pre-establishment wait, whether queued or promoted
  pending acceptance; `hold(media)` is negotiated hold.
- Apply public transitions only through the core `CallStateMachine`; PJSUA
  callback translation must not duplicate its transition table.
- Publish one copied terminal transition before immediate handle invalidation
  and slot return to `idle`; do not retain terminal contexts on a timer.

---

## Task 1: Add native call contexts and callback-safe mapping

**Files:**

- Create: `voip/src/pjsua/PjsuaCallContext.hpp`
- Create: `voip/src/pjsua/PjsuaCallManager.hpp`
- Create: `voip/src/pjsua/PjsuaCallManager.cpp`
- Create: `voip/tests/pjsua/PjsuaCallContextTest.cpp`
- Modify: `voip/src/pjsua/PjsuaCallbackRouter.hpp`
- Modify: `voip/src/pjsua/PjsuaCallbackRouter.cpp`

**Interfaces:**

- Consumes: `CallHandle`, `AgentHandle`, `pjsua_call_id`, PJSUA call user data.
- Produces: stable seven-record native-call lookup and copied callback notifications.

- [ ] Write tests allocating seven contexts, binding native IDs in non-slot order, resolving via user data, rejecting duplicate IDs, clearing user data before reuse, ignoring late callbacks from a released generation, and exhausting the eighth context.

- [ ] Confirm tests fail because the call context manager is absent.

- [ ] Define each context with fixed storage: logical handle, agent handle,
  optional PJSUA ID, private `PjsuaCallPhase`, incoming/outgoing direction,
  answer-on-promotion flag, local operation IDs, queue/answer deadlines,
  copied native callback data, and teardown/quiescence flags. Store no public
  `CallState`, `HoldReason`, application-owned audio pointer, or second business
  state machine here; the core logical context is the single source.

- [ ] Bind native calls with `pjsua_call_set_user_data(call_id, &stable_context)` only after the logical context is committed. Clear it before releasing the context; verify `pjsua_call_get_user_data()` in callbacks and reject mismatches.

- [ ] Copy all callback data needed later while inside the callback. Never retain `pjsip_rx_data*`, `pjsua_call_info` string views, SDP pool pointers, or callback-local memory.

- [ ] Extend the callback router to dispatch incoming, call-state, and media-state callbacks to `PjsuaCallManager`; never invoke the public event consumer directly.

- [ ] Run `PjsuaCallContextTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua/PjsuaCallContextTest.cpp
  git commit -m "feat(voip): add bounded pjsua call contexts"
  ```

## Task 2: Admit incoming calls and preserve early dialogs in FIFO

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/core/CallScheduler.hpp`
- Modify: `voip/src/core/CallScheduler.cpp`
- Create: `voip/tests/pjsua/PjsuaIncomingAdmissionTest.cpp`

**Interfaces:**

- Consumes: `on_incoming_call(acc_id, call_id, rdata)`, account mapping, logical call/event reservations.
- Produces: incoming call handle/event, 180 response, immediate promotion or queued-incoming state.

- [ ] Write fake tests for unknown account (404), no logical/event capacity (486), immediate admission, same-agent queueing, two globally promoted agents plus five queued incoming calls, eighth rejection (486), and failure to send 180.

- [ ] Add a test proving invisible admission cannot occur: if guaranteed incoming-event reservation fails, `pjsua_call_answer(...,486,...)` is sent and no logical/native context remains bound.

- [ ] Confirm tests fail against the empty callback handler.

- [ ] Resolve `pjsua_acc_id -> PjsuaAccountContext -> AgentHandle`. Return 404 only for an unknown/unconfigured account mapping.

- [ ] Reserve logical call context and guaranteed incoming event before committing. Bind the provided native ID, call `pjsua_call_answer2(call_id, ..., 180, ..., nullptr)`, and roll back with a final failure response if 180 cannot be applied.

- [ ] Ask `CallScheduler` for immediate promotion or FIFO insertion. A queued entry retains its native ID but no media bridge. Populate the event with full agent/call snapshot and queue position.

- [ ] Project admission through `initiated`; immediate acceptance may continue
  to `established`, while FIFO insertion publishes `initiated -> hold` with
  `HoldReason::waiting`. The waiting state must not allocate a media bridge.

- [ ] When the scheduler/PJSUA capacity is full, respond `PJSIP_SC_BUSY_HERE` (486). Do not use 480 for capacity rejection.

- [ ] Run `PjsuaIncomingAdmissionTest` and require all SIP-code and ownership assertions pass.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaCallManager.* voip/src/core/CallScheduler.* voip/tests/pjsua/PjsuaIncomingAdmissionTest.cpp
  git commit -m "feat(voip): admit and queue incoming PJSUA calls"
  ```

## Task 3: Promote outgoing requests only when eligible

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Modify: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Create: `voip/tests/pjsua/PjsuaOutgoingPromotionTest.cpp`

**Interfaces:**

- Consumes: accepted `Dial` operation and scheduler promotion notifications.
- Produces: delayed `pjsua_call_make_call()` and native ID binding.

- [ ] Write tests showing an unregistered agent returns `agent_unavailable` without operation ID, immediate dial calls PJSUA once, queued dial calls it zero times, cancellation before promotion calls it zero times, and promotion later calls it once with the original copied URI.

- [ ] Add failure tests for PJSUA call-ID exhaustion and INVITE creation failure: the dial operation completes with mapped failure, the promoted lease is torn down, terminal call event is published, and the FIFO advances.

- [ ] Confirm tests fail because outgoing promotion is not wired.

- [ ] On `Dial`, validate agent registration, URI, logical capacity, FIFO capacity, operation reservation, and terminal-event reservation. Create the logical handle before returning. Copy the URI into `CallContext`.

- [ ] If queued, complete the dial operation as locally admitted and publish
  `hold(waiting)` without calling PJSUA. Network establishment remains a later
  call event.

- [ ] On promotion, build `pjsua_call_setting` with one audio media line, video count zero, no SRTP requirement, and account-specific options; call `pjsua_call_make_call(account_id, &uri, &setting, context, ..., &call_id)` and bind the returned ID.

- [ ] Treat promotion as a state transition, not a second public operation. Map immediate PJ errors to `signaling_failed`, `negotiation_failed`, or `resource_exhausted` without leaking PJ status text containing sensitive values.

- [ ] On promotion, change only private phase and lease ownership; retain
  `hold(waiting)` until acceptance/confirmation projects directly to
  `established`. A promotion is not a public resume operation and must not
  synthesize a `hold -> initiated` edge absent from the UML.

- [ ] Run `PjsuaOutgoingPromotionTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaCallManager.* voip/src/pjsua/PjsuaRuntimeAdapter.cpp voip/tests/pjsua/PjsuaOutgoingPromotionTest.cpp
  git commit -m "feat(voip): promote queued outgoing calls through PJSUA"
  ```

## Task 4: Implement answer, reject, cancel, hangup, hold, and timers

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Create: `voip/tests/pjsua/PjsuaCallControlTest.cpp`

**Interfaces:**

- Consumes: actor-owned control commands and queue/answer deadlines.
- Produces: correct PJSUA calls, operation terminal events, state transitions.

- [ ] Write a state/command matrix covering valid and invalid commands for queued outgoing, queued incoming, incoming, outgoing/early, established, held, disconnecting, and terminal contexts.

- [ ] For every matrix row assert both the private phase and the public
  projection. In particular, `Answer(hold(waiting))` is valid only for a
  queued incoming phase, while `SetHeld(false)` is valid only for a promoted
  held phase with `HoldReason::media`.

- [ ] Add explicit tests: answer queued incoming sets a flag and sends no 200 until promotion; reject queued incoming sends selected 4xx/6xx and removes it; cancel queued outgoing sends no SIP; remote CANCEL wins over queued answer; queue timeout sends 480; promoted answer timeout rejects; hangup sends final PJSUA hangup; hold/unhold call the corresponding PJSUA APIs.

- [ ] Confirm failures against unimplemented controls.

- [ ] Implement command behavior:

  - `Answer(queued_incoming)`: set `answer_on_promotion`, complete operation locally.
  - `Answer(incoming)`: `pjsua_call_answer2(...,200,...)`.
  - `Reject(queued/promoted incoming)`: validate status 400–699, answer with it, begin teardown.
  - `Cancel(queued_outgoing)`: remove locally; `Cancel(outgoing/early)`: hang up using 487-compatible cancellation behavior.
  - `Hangup(established/held)`: `pjsua_call_hangup()`.
  - `SetHeld(true)`: `pjsua_call_set_hold2()`; false: `pjsua_call_reinvite2()` with unhold flags.

- [ ] On queued-incoming promotion, send 200 immediately if `answer_on_promotion`; otherwise enter `incoming` and start `answer_timeout_ms`. Queue deadline starts at admission for both directions.

- [ ] A queue or answer timeout publishes its guaranteed terminal transition
  before cleanup. The context is then invalidated immediately and the copied
  event remains readable; there is no terminated-context retention timer.

- [ ] Each accepted public operation produces exactly one terminal operation event regardless of later network state. Stale/invalid commands are rejected before allocation.

- [ ] Run `PjsuaCallControlTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaCallManager.* voip/tests/pjsua/PjsuaCallControlTest.cpp
  git commit -m "feat(voip): add queued and promoted call control"
  ```

## Task 5: Translate PJSUA call state and release slots after quiescent teardown

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallManager.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Create: `voip/tests/pjsua/PjsuaCallStateTest.cpp`

**Interfaces:**

- Consumes: `on_call_state`, `on_call_media_state`, remote status/cause.
- Produces: public snapshots/events and scheduler teardown-complete notification.

- [ ] Write callback-sequence tests for outgoing calling/early/confirmed/disconnected, incoming confirmed, remote rejection, remote CANCEL, transport loss, duplicate callbacks, and a late callback after logical generation reuse. Assert the exact public business-state and cause trace for each sequence.

- [ ] Add the release-order test: disconnected callback arrives while media is not quiescent; the promoted slot remains held. Media stop/quiescence then arrives; native user data is cleared, context released, terminal event queued, and only then does the scheduler promote the FIFO head.

- [ ] Confirm tests fail before implementing state translation.

- [ ] Call `pjsua_call_get_info()` inside the callback and copy remote URI, state, role, status code, media direction, and sanitized reason into the fixed public snapshot.

- [ ] Map PJSUA states to product states; classify 3xx–6xx final responses as `remote_rejected`, local/remote cancellation as `cancelled`, timeout as `timed_out`, SDP failure as `negotiation_failed`, and transport failure as `signaling_failed`.

- [ ] Keep PJSUA calling, incoming, early, disconnecting, and quiescence as
  private phases. Submit confirmation, successful media hold, and successful
  resume as transition causes to the core `CallStateMachine`. A failed hold or
  resume submits no transition and preserves the prior public projection.

- [ ] Make terminal publication idempotent. Do not release the promoted lease until signaling is disconnected, media reports stopped, callback counter is zero, and future security cleanup flag is complete.

- [ ] Clear call user data before invalidating the logical handle. Publish one
  guaranteed terminal transition containing source state, destination state,
  cause, outcome, and complete final snapshot; then release/increment the
  generation, return the slot to `idle` immediately, and invoke scheduler
  capacity handling. Verify `GetCallSnapshot(old_handle)` already returns
  `invalid_handle` while the copied event is still pollable.

- [ ] Run `PjsuaCallStateTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaCallManager.* voip/tests/pjsua/PjsuaCallStateTest.cpp
  git commit -m "feat(voip): translate PJSUA call lifecycle safely"
  ```

## Task 6: Validate two promoted plus five queued calls over SIP TCP

**Files:**

- Create: `applications/voip_integration/src/multi_call_scheduler.cpp`
- Create: `applications/voip_integration/multi_call_scheduler.conf`
- Create: `applications/voip_integration/test_support/ScriptedSipPeer.hpp`
- Create: `applications/voip_integration/test_support/ScriptedSipPeer.cpp`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: five registered agents, public call API, localhost scripted SIP peers.
- Produces: network-level FIFO/promotion/SIP-response acceptance proof.

- [ ] Script five accounts and seven calls with mixed directions. Establish promoted calls for agents A and B; enqueue incoming A, outgoing C, incoming D, outgoing E, and incoming B in that exact order.

- [ ] Verify only two calls have sent/accepted final INVITE signaling, queued incoming calls received 180, and queued outgoing C/E produced no INVITE.

- [ ] End B while A remains busy. Verify the free global slot stays idle because queued A is at the head. End A; verify queued A promotes first, then only after it ends does outgoing C send its first INVITE.

- [ ] Fill all seven PJSUA call records with two promoted plus five queued incoming calls and send an eighth incoming INVITE; verify 486. Separately expire a queued incoming call and verify 480.

- [ ] Verify answer-on-promotion, reject while queued, cancel queued outgoing, remote CANCEL, hold/unhold, and per-agent one-promoted-call invariant.

- [ ] Verify the network scenario produces the normative business trace:
  initiation, `hold(waiting)` while queued, acceptance to `established`,
  `hold(media)`/resume where requested, finish or rejection to `terminated`,
  terminal publication, and immediate cleanup to `idle`.

- [ ] Build and run:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_multi_call -- \
    -DEXTRA_CONF_FILE=multi_call_scheduler.conf
  west build -d build_voip_multi_call -t run
  ```

  Expected terminator: `VOIP MULTI CALL RESULT: PASSED (2 promoted, 5 FIFO)`.

- [ ] Repeat the mixed scenario for five lifecycles and verify no live calls, contexts, operations, event reservations, or arena blocks remain after shutdown.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): validate PJSUA call promotion and FIFO"
  ```

## Plan 4 Exit Criteria

- Incoming calls map account ID to the correct agent and receive visible admission or a final rejection.
- Queued incoming calls retain early PJSUA state; queued outgoing calls do not allocate/send.
- PJSUA phases project through the core UML state machine without duplicating
  or bypassing its transition rules.
- Exactly two calls can be promoted globally and only one for each agent.
- Strict shared FIFO and head-of-line blocking are proven at unit and SIP levels.
- Answer/reject/cancel/hangup/hold and both timeouts behave as specified.
- Terminal state is published once, and slot reuse waits for signaling/media/callback quiescence.
- After terminal publication, the old handle is invalid immediately while its
  copied terminal event remains pollable.
