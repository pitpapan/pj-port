# VoIP Core Service and FIFO Scheduler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the compatibility facade with a heap-free, PJ-independent public service supporting five immutable agents, seven logical calls, two promoted slots, a strict five-entry FIFO, asynchronous operations, and polling events.

**Architecture:** Application threads copy commands into a fixed mailbox. One runtime actor owns domain state and applies commands through an injectable private runtime adapter. Handles are generation checked. Accepted operations reserve a terminal event before admission. A fixed event ring coalesces snapshots, permanently reserves the service-stopped record, and exposes only polling APIs.

**Tech Stack:** C++17, Zephyr synchronization primitives behind private adapters, fixed arrays/rings, placement construction in service-owned storage, standalone host unit tests, Zephyr CMake/Kconfig.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Business state machine:** `docs/superpowers/specs/state_machine.uml`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- This plan must compile and test without PJPROJECT.
- One to five agents are copied only during `Initialize()` and remain immutable.
- Use capacities: agents 5, logical calls 7, promoted calls 2, pending FIFO 5, commands 16, operations 16, events 32.
- Allocate no C++ object from the heap after construction or initialization.
- Preserve strict FIFO head-of-line blocking.
- Replace callback delivery and `k_work_q` exposure with `TryGetEvent()` and `WaitForEvent()`.
- Expose only the five business states `idle`, `initiated`, `established`,
  `hold`, and `terminated`; keep scheduler/PJSUA phases private.
- Distinguish pre-establishment `hold(waiting)` from negotiated
  `hold(media)` with `HoldReason`; use private phase for FIFO/lease ownership.
- Publish the copied terminal transition before immediately invalidating the
  handle and returning the slot to `idle`; add no terminal-retention timer.

---

## Task 1: Publish the breaking, PJ-independent contract

**Files:**

- Create: `voip/include/voip/VoipTypes.hpp`
- Create: `voip/include/voip/PcmAudio.hpp`
- Create: `voip/include/voip/VoipEvents.hpp`
- Replace: `voip/include/voip/VoipService.hpp`
- Create: `voip/tests/unit/PublicContractTest.cpp`

**Interfaces:**

- Consumes: approved public API in the architecture specification.
- Produces: `AgentHandle`, `CallHandle`, `OperationId`, `CallState`,
  `HoldReason`, `CallTransition`, `AgentConfig`, `ServiceConfig`, `DialRequest`,
  snapshots, polling events, `PcmSource`, and `PcmSink`.

- [ ] Write `PublicContractTest.cpp` first. It must compile a one-agent `ServiceConfig`, verify handles are trivially copyable, verify no public header includes `pjsua`, `pjsip`, or Zephyr workqueue types, and assert all public strings/snapshots have fixed maximum sizes.

- [ ] Assert the public call-state contract contains exactly
  `CallState::{idle,initiated,established,hold,terminated}` plus
  `HoldReason::{none,waiting,media}` and the UML transition causes. Verify no
  queued, SIP, media, or teardown phase is exposed in a public header.

- [ ] Compile against the current headers and confirm failure because `AgentConfig`, `TryGetEvent()`, and `WaitForEvent()` do not exist:

  ```sh
  c++ -std=c++17 -Wall -Wextra -Werror -Ivoip/include \
    voip/tests/unit/PublicContractTest.cpp -c -o /tmp/voip-public-contract.o
  ```

- [ ] Define `Error` with exactly these product categories: `ok`, `invalid_argument`, `invalid_handle`, `invalid_state`, `unsupported_configuration`, `agent_unavailable`, `busy`, `queue_full`, `resource_exhausted`, `authentication_failed`, `signaling_failed`, `remote_rejected`, `negotiation_failed`, `media_failed`, `cancelled`, `timed_out`, `shutting_down`, `shutdown_timeout`, and `internal_failure`.

- [ ] Define fixed handles and format-aware audio:

  ```cpp
  struct AgentHandle { std::uint8_t slot; std::uint16_t generation; };
  struct CallHandle  { std::uint8_t slot; std::uint16_t generation; };
  using OperationId = std::uint32_t;

  struct PcmFormat {
      std::uint32_t sample_rate_hz;
      std::uint16_t samples_per_frame;
      std::uint8_t channels;
      SampleFormat sample_format;
  };
  ```

- [ ] Define `PcmSource::Format()`/`Read()` and `PcmSink::Format()`/`Write()`/`Flush()` as non-blocking `noexcept` methods using fixed `int16_t` frame buffers and timestamps. Document that objects are borrowed from successful initialization through completed shutdown. `Flush()` discards stale playout data without freeing storage.

- [ ] Define `SignalingSecurity::{none,tls}` and `MediaSecurity::{none,srtp_sdes}` now, but accept only `{none,none}` until their build gates exist.

- [ ] Define `CallState`, `HoldReason`, and `CallTransition` exactly as the
  approved spec. Every `CallSnapshot` carries state and hold reason; every
  call-state event carries source state, destination state, transition cause,
  and a copied full snapshot. `cleanup` is recorded only by internal diagnostic
  traces and does not enqueue a second event after terminal publication.

- [ ] Define `AgentAudioBinding`, `SipAccountConfig`, `AgentConfig`, and `ServiceConfig` exactly as the approved spec, including `conference_format`, queue/answer timeouts, and `register_on_start`.

- [ ] Define `VoipService` with the approved control/snapshot/event methods. Remove the `Backend` constructor, `EventHandler`, and workqueue parameter. Use a private aligned byte-storage member and placement construction in `VoipService.cpp`; add a `static_assert(sizeof(Impl) <= sizeof(storage_))` so no `new` is required.

- [ ] Re-run the compile-only contract test and require success.

- [ ] Commit:

  ```sh
  git add voip/include/voip voip/tests/unit/PublicContractTest.cpp
  git commit -m "feat(voip): publish multi-agent polling API"
  ```

## Task 2: Implement generation-safe fixed handle pools

**Files:**

- Replace: `voip/src/HandlePool.hpp`
- Create: `voip/tests/unit/HandlePoolTest.cpp`

**Interfaces:**

- Consumes: `AgentHandle`, `CallHandle`.
- Produces: `HandlePool<Handle, Capacity>::Allocate()`, `Resolve()`, `Release()`, `InvalidateAll()`.

- [ ] Write tests for full allocation, exhaustion, stale handle rejection, generation increment after release, generation wrap skipping zero, and invalidation across shutdown/reinitialize.

- [ ] Run the old implementation against the new tests and confirm at least invalidation/wrap tests fail.

- [ ] Implement a templated fixed slot array containing occupancy, generation, and object storage. `Allocate()` returns the lowest free slot; `Resolve()` checks bounds, occupancy, and generation; `Release()` destroys the object and advances the generation without producing zero.

- [ ] Add compile-time assertions that agent capacity is 5 and call capacity is 7 at their instantiation sites.

- [ ] Run:

  ```sh
  c++ -std=c++17 -Wall -Wextra -Werror -Ivoip/include -Ivoip/src \
    voip/tests/unit/HandlePoolTest.cpp -o /tmp/voip-handle-test && \
    /tmp/voip-handle-test
  ```

  Expected: `HandlePoolTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/HandlePool.hpp voip/tests/unit/HandlePoolTest.cpp
  git commit -m "feat(voip): add generation-safe fixed handles"
  ```

## Task 3: Copy and validate immutable agent/audio configuration

**Files:**

- Create: `voip/src/core/AgentContext.hpp`
- Create: `voip/src/core/AgentRegistry.hpp`
- Create: `voip/src/core/AgentRegistry.cpp`
- Create: `voip/tests/unit/AgentRegistryTest.cpp`

**Interfaces:**

- Consumes: `ServiceConfig`, `AgentConfig`, borrowed `PcmSource*`/`PcmSink*`.
- Produces: `AgentRegistry`, `AgentContext`, owned SIP strings, stable config-index handles.

- [ ] Write tests for zero/six agents, null configuration, null/oversized SIP strings, duplicate identity URI, null audio endpoint, invalid PCM format, unsupported TLS/SRTP, copied caller strings, and stable config-index handle mapping.

- [ ] Confirm tests fail because `AgentRegistry` is absent.

- [ ] Define `AgentContext` with `AgentHandle handle`, fixed owned SIP fields, copied `AgentAudioBinding`, registration snapshot, and optional promoted `CallHandle`. Do not place `pjsua_acc_id` or PJ types here.

- [ ] Implement a two-pass initializer: validate every agent and security policy without mutation; then copy all fields into the five-slot registry. If any validation fails, erase copied credential arrays and leave the registry empty.

- [ ] Return `unsupported_configuration` when TLS or SRTP is selected while disabled. Return `invalid_argument` for zero agents, more than five agents, null audio bindings, invalid URI/credential lengths, or an unsupported PCM shape.

- [ ] Run the host test and require `AgentRegistryTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/core/AgentContext.hpp voip/src/core/AgentRegistry.* voip/tests/unit/AgentRegistryTest.cpp
  git commit -m "feat(voip): add immutable agent registry"
  ```

## Task 4: Add the operation table and command mailbox

**Files:**

- Create: `voip/src/core/VoipCommand.hpp`
- Create: `voip/src/core/CommandMailbox.hpp`
- Create: `voip/src/core/OperationTable.hpp`
- Create: `voip/src/core/OperationTable.cpp`
- Create: `voip/tests/unit/OperationMailboxTest.cpp`

**Interfaces:**

- Consumes: public command arguments and handles.
- Produces: copied 16-record mailbox, 16-record operation table, nonzero monotonic operation IDs.

- [ ] Write tests that fill both capacities, verify rejection does not consume an operation ID, verify copied dial URIs survive caller-buffer mutation, reject stale handles before enqueue, and wrap operation ID from `UINT32_MAX` to 1 without collision.

- [ ] Confirm compilation fails because the types are absent.

- [ ] Define a tagged `VoipCommand` union with no owning pointer to caller stack: `dial`, `answer`, `reject`, `cancel`, `hangup`, `set_held`, and `shutdown`. Store URI/reason data in fixed arrays.

- [ ] Implement a multi-producer/single-consumer fixed ring behind a small synchronization adapter. Admission order is: validate arguments/handle, reserve operation record, reserve terminal event, copy command, publish command. Roll back reservations in reverse order on failure.

- [ ] Store shutdown completion synchronization inside the service implementation, never inside a caller stack frame, preventing the current `RunSync()` timeout use-after-return class.

- [ ] Run the unit test and require `OperationMailboxTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/tests/unit/OperationMailboxTest.cpp
  git commit -m "feat(voip): add bounded commands and operations"
  ```

## Task 5: Implement the polling event queue

**Files:**

- Create: `voip/src/core/VoipEventQueue.hpp`
- Create: `voip/src/core/VoipEventQueue.cpp`
- Create: `voip/tests/unit/VoipEventQueueTest.cpp`

**Interfaces:**

- Consumes: complete copied `Event` records.
- Produces: 32-record queue, reservation tokens, monotonic sequence numbers, `TryPop()` and timed `WaitPop()`.

- [ ] Write tests for FIFO delivery, empty nonblocking return, timeout, one polling consumer, guaranteed-event reservations, coalescing replacement by handle/type, sequence monotonicity, and the permanently reserved `service_stopped` slot.

- [ ] Add a pressure test filling all ordinary capacity. Verify a new guaranteed incoming event cannot be reserved, but `service_stopped` can still be enqueued and remains the final produced event.

- [ ] Confirm tests fail due to the missing queue.

- [ ] Implement an array ring plus reservation counters. Coalescible insertion scans the at-most-32 pending records and replaces the matching event snapshot without changing its queue position; it assigns the replacement a new sequence number.

- [ ] Signal one private semaphore after publishing an event. `TryPop()` never blocks. `WaitPop()` waits at most `timeout_ms`, then rechecks under the queue lock. Neither path invokes application code or PJPROJECT.

- [ ] Define guaranteed types as incoming admission, operation terminal, call terminal, registration failure, and service stopped; reserve the stopped slot for the full initialized lifecycle.

- [ ] Run the unit test and require `VoipEventQueueTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/core/VoipEventQueue.* voip/tests/unit/VoipEventQueueTest.cpp
  git commit -m "feat(voip): add fixed polling event queue"
  ```

## Task 6: Implement strict FIFO scheduling and logical call contexts

**Files:**

- Create: `voip/src/core/CallContext.hpp`
- Create: `voip/src/core/CallStateMachine.hpp`
- Create: `voip/src/core/CallStateMachine.cpp`
- Create: `voip/src/core/CallScheduler.hpp`
- Create: `voip/src/core/CallScheduler.cpp`
- Create: `voip/tests/unit/CallStateMachineTest.cpp`
- Create: `voip/tests/unit/CallSchedulerTest.cpp`

**Interfaces:**

- Consumes: admitted incoming/outgoing logical calls and agent busy state.
- Produces: seven logical contexts, seven normative business-state machines,
  two promoted leases, and one shared five-entry strict FIFO.

- [ ] Write `CallStateMachineTest.cpp` as a literal transition table covering
  every edge in `state_machine.uml`, both `hold` reasons, and invalid edges.
  Prove `acceptance` is valid from `initiated` and `hold(waiting)`, while
  `resume` is valid only from `hold(media)`. Prove direct initiated timeout and
  terminated cleanup both return `idle`, but only the former is terminal-event
  bearing.

- [ ] Write table-driven scheduler tests for immediate promotion, two different
  agents active, same-agent queueing, mixed incoming/outgoing order, FIFO full,
  cancel from middle/head, timeout removal, repeated promotion after teardown,
  and the exact business-state traces from `state_machine.uml`.

- [ ] Add the required head-of-line test: agent A is promoted; queue head targets A; a later entry targets idle B; one global slot is free. Verify B does not bypass A.

- [ ] Confirm tests fail because the scheduler does not exist.

- [ ] Define private `LogicalCallPhase` values `queued_outgoing`, `queued_incoming`,
  `promoting`, `outgoing`, `incoming`, `early`, `established`, `held`,
  `disconnecting`, and terminal categories. Store one `CallStateMachine`
  instance in each `CallContext`; do not mirror its `CallState` or
  `HoldReason` in scheduler or adapter records. A queued outgoing context
  stores a copied URI but no native call ID; a queued incoming context may
  carry an opaque runtime call token.

- [ ] Implement the PJ-independent state machine with this private core
  contract; `Apply()` rejects edges not present in the UML without mutating the
  previous projection:

  ```cpp
  struct CallProjection {
      CallState state;
      HoldReason hold_reason;
  };

  struct AppliedCallTransition {
      CallProjection before;
      CallProjection after;
      CallTransition cause;
      bool terminal_event_required;
  };

  class CallStateMachine final {
  public:
      CallProjection Snapshot() const noexcept;
      Error Apply(CallTransition,
                  AppliedCallTransition *) noexcept;
  };
  ```

- [ ] Implement one exhaustive phase/cause projector. FIFO insertion produces
  `hold(waiting)`; promotion alone retains it until acceptance; confirmed
  signaling produces `established`; successful SDP hold produces
  `hold(media)`. Never use the five-state projection to decide whether a
  PJSUA ID, promoted lease, timer, or media bridge exists.

- [ ] Implement `CanPromote(agent)` as `promoted_count < 2 && !agent.promoted_call.IsValid()`. New calls promote immediately only when the FIFO is empty; otherwise append.

- [ ] Implement `OnCapacityChanged()` to inspect only the head and repeatedly promote while eligible. Never scan for another agent.

- [ ] On logical release, remove the FIFO entry if present and release any
  promoted lease only after a runtime teardown-complete notification. Reserve
  and publish the copied terminal transition first, then invalidate the handle,
  transition the slot to `idle` without a retention timer, and rerun promotion.

- [ ] Run both unit tests and require `CallStateMachineTest PASSED` and
  `CallSchedulerTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/core/CallContext.hpp voip/src/core/CallStateMachine.* \
    voip/src/core/CallScheduler.* voip/tests/unit/CallStateMachineTest.cpp \
    voip/tests/unit/CallSchedulerTest.cpp
  git commit -m "feat(voip): add strict shared FIFO scheduler"
  ```

## Task 7: Compose the core runtime with a deterministic fake adapter

**Files:**

- Create: `voip/src/core/RuntimeAdapter.hpp`
- Create: `voip/src/core/VoipRuntime.hpp`
- Create: `voip/src/core/VoipRuntime.cpp`
- Replace: `voip/src/VoipService.cpp`
- Create: `voip/tests/unit/FakeRuntimeAdapter.hpp`
- Create: `voip/tests/unit/VoipServiceCoreTest.cpp`
- Create: `voip/tests/run_host_tests.sh`
- Create: `applications/voip_integration/src/core_service.cpp`
- Create: `applications/voip_integration/core_service.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`

**Interfaces:**

- Consumes: registry, mailbox, operations, event queue, scheduler.
- Produces: functional public service without PJPROJECT; private adapter seam for Plans 3–5.

- [ ] Write end-to-end host tests initializing five agents, retrieving handles by config index, dialing seven logical calls, observing two promotions/five FIFO positions, cancelling entries, consuming terminal operation events, rejecting unsupported security, and shutting down twice safely.

- [ ] Add tests proving one agent never owns two promoted calls and that event polling never invokes callbacks.

- [ ] Add end-to-end trace assertions for initiation, FIFO wait, acceptance,
  rejection, timeout, media hold/resume, finish, terminal publication, and
  immediate stale-handle rejection after cleanup. Verify the terminal event
  remains readable after the handle becomes invalid.

- [ ] Confirm the new contract does not link against the current `VoipService.cpp`.

- [ ] Define `RuntimeAdapter` methods for account initialization, promote outgoing, promote incoming, answer/reject/cancel/hangup/hold, poll native events, begin call teardown, and shutdown. Use opaque integer tokens only; no PJ type crosses into core headers.

- [ ] Implement `VoipRuntime::Step(now_ms)` to drain bounded commands, apply timers, run strict promotion, poll the adapter, update snapshots, and publish events. The Zephyr actor repeatedly calls `Step()`; host tests call it deterministically.

- [ ] Implement public APIs as validation/copy/admission only. They must not synchronously wait for operation completion. Implement synchronous `Shutdown()` with service-owned completion state and bounded timeout.

- [ ] Add fixed Kconfig constants with compile-time checks: 5/7/2/5/16/16/32. Add `CONFIG_VOIP_SERVICE`, retain `CONFIG_VOIP_PJSUA` for the production adapter, and make old/new lower-case backends mutually exclusive.

- [ ] Run all Plan 2 host tests with one command documented in `voip/tests/run_host_tests.sh`; the script may compile into `/tmp` and must not write generated artifacts into the source tree.

- [ ] Build the fake-only integration image with `west build`; run it and require `VOIP CORE SERVICE RESULT: PASSED`.

- [ ] Commit:

  ```sh
  git add voip applications/voip_integration
  git commit -m "feat(voip): compose bounded multi-agent core service"
  ```

## Plan 2 Exit Criteria

- Public API exposes immutable agent/audio initialization, asynchronous control, snapshots, and polling events only.
- `CallStateMachine` implements every UML edge, rejects every tested invalid
  edge, and keeps `hold(waiting)` distinct from `hold(media)`.
- Five agents and seven logical calls are generation safe.
- Two global promoted slots and one five-entry strict FIFO behave exactly as specified.
- Same-agent and FIFO head-of-line rules are tested.
- Every accepted operation owns one terminal-event reservation.
- `service_stopped` is always deliverable and final.
- Terminal call events remain readable after immediate handle invalidation and
  slot return to `idle`.
- The core passes without linking PJPROJECT or allocating post-init heap objects.
