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

## Execution Order and Review Gates

Execute tasks strictly in numeric order.  The dependency chain is:

```text
public contract
    -> generation-safe handles
    -> immutable agent registry
    -> event reservations and polling queue
    -> operations and command mailbox
    -> call state machine and FIFO scheduler
    -> core runtime and fake adapter
    -> Zephyr integration image
```

For every task:

1. Record the task base commit.
2. Add the smallest behavior test that fails for the expected missing behavior.
3. Run that focused test and record its RED failure in the task report.
4. Add only the production code needed for GREEN.
5. Run the focused test, every earlier Plan 2 host test, and `git diff --check`.
6. Commit the task and write its report in the Plan 2 SDD workspace.
7. Pass the complete base-to-head diff through independent specification and
   code-quality review before starting the next task.

Host binaries and generated objects must be written under explicit `/tmp/`
paths.  No task may place build output in `voip/` or inspect `zephyr/`.

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

- [ ] Write `PublicContractTest.cpp` first. It must compile a one-agent
  `ServiceConfig`, verify handles are trivially copyable, verify no public
  header includes `pjsua`, `pjsip`, or Zephyr workqueue types, and assert the
  retained embedded maxima: URI 255 characters, username 63, password 127,
  sanitized reason 95, and address 63, each plus one terminator in owned
  arrays.

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

- [ ] Define `RegistrationState` with exactly `disabled`, `registering`,
  `registered`, `refreshing`, `retry_wait`, `unregistering`,
  `authentication_failed`, and `transport_failed`.  Public events distinguish
  agent snapshots, incoming admission, call transitions, operation terminal,
  media/resource snapshots, and `service_stopped`; they never expose a PJ
  pointer or borrowed callback string.

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

- [ ] Define `VoipService` with the approved control/snapshot/event methods.
  Remove the `Backend` constructor, `EventHandler`, and workqueue parameter.
  Reserve `131072` bytes of private `alignas(std::max_align_t)` implementation
  storage and retain only an `Impl*` pointing inside it.  Task 7 owns placement
  construction and the `static_assert(sizeof(Impl) <= sizeof(storage_))` once
  `Impl` is complete.  Construction, initialization, and shutdown must never
  call `new` for the implementation object.  Plan 6 may reduce this initial
  storage budget only after target measurement.

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

- [ ] Write tests for zero/six agents, null configuration, null/oversized SIP
  strings, duplicate identity URI, duplicate source or sink binding, null audio
  endpoint, invalid PCM format, unsupported TLS/SRTP, copied caller strings,
  and stable config-index handle mapping.

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

## Task 4: Implement synchronization and the polling event queue

**Files:**

- Create: `voip/src/core/CoreSynchronization.hpp`
- Create: `voip/src/core/VoipEventQueue.hpp`
- Create: `voip/src/core/VoipEventQueue.cpp`
- Create: `voip/tests/unit/VoipEventQueueTest.cpp`

**Interfaces:**

- Consumes: complete copied `Event` records and the public timeout contract.
- Produces: 32-record queue, reservation tokens, monotonic sequence numbers, `TryPop()` and timed `WaitPop()`.

- [ ] Write tests for FIFO delivery, empty nonblocking return, zero/nonzero
  timeout, one polling consumer, guaranteed-event reservations, reservation
  commit/cancel exactly once, coalescing replacement by handle/type, sequence
  monotonicity, and the permanently reserved `service_stopped` slot.

- [ ] Add a pressure test filling all ordinary capacity. Verify a new guaranteed incoming event cannot be reserved, but `service_stopped` can still be enqueued and remains the final produced event.

- [ ] Confirm tests fail due to the missing queue.

- [ ] Define `CoreMutex`, `CoreLockGuard`, and `CoreEventSignal` in one private
  header.  The host branch uses `std::mutex` plus `std::condition_variable`;
  the `__ZEPHYR__` branch uses documented kernel mutex/semaphore APIs.  The
  wrapper owns all synchronization storage directly, exposes no native type,
  and performs no allocation after construction.  Do not read Zephyr source.

- [ ] Implement an array ring plus reservation counters. Coalescible insertion scans the at-most-32 pending records and replaces the matching event snapshot without changing its queue position; it assigns the replacement a new sequence number.

- [ ] Signal one private semaphore after publishing an event. `TryPop()` never blocks. `WaitPop()` waits at most `timeout_ms`, then rechecks under the queue lock. Neither path invokes application code or PJPROJECT.

- [ ] Define guaranteed types as incoming admission, operation terminal, call terminal, registration failure, and service stopped; reserve the stopped slot for the full initialized lifecycle.

- [ ] Run the host test with `-pthread` and require
  `VoipEventQueueTest PASSED`.  Repeat timeout and producer/consumer tests at
  least 100 times inside the binary so a lost wakeup fails deterministically.

- [ ] Commit:

  ```sh
  git add voip/src/core/CoreSynchronization.hpp \
    voip/src/core/VoipEventQueue.* voip/tests/unit/VoipEventQueueTest.cpp
  git commit -m "feat(voip): add fixed polling event queue"
  ```

## Task 5: Add the operation table and command mailbox

**Files:**

- Create: `voip/src/core/VoipCommand.hpp`
- Create: `voip/src/core/CommandMailbox.hpp`
- Create: `voip/src/core/OperationTable.hpp`
- Create: `voip/src/core/OperationTable.cpp`
- Create: `voip/tests/unit/OperationMailboxTest.cpp`

**Interfaces:**

- Consumes: public command arguments, handles, `CoreMutex`, and
  `VoipEventQueue::Reservation`.
- Produces: copied 16-record mailbox, 16-record operation table, nonzero
  monotonic operation IDs, and exactly one owned terminal-event reservation
  for every accepted operation.

- [ ] Write tests that fill both capacities, verify rejection does not consume
  an operation ID, verify copied dial URIs survive caller-buffer mutation,
  reject stale handles before enqueue, and wrap operation ID from `UINT32_MAX`
  to 1 without collision.

- [ ] Add rollback tests for operation-table full, event-reservation failure,
  and mailbox full.  After each failure, assert all earlier reservations are
  restored and the next accepted command receives exactly one terminal event.

- [ ] Confirm compilation fails because the types are absent.

- [ ] Define a tagged `VoipCommand` union with no owning pointer to caller
  stack: `dial`, `answer`, `reject`, `cancel`, `hangup`, `set_held`, and
  `shutdown`. Store URI/reason data in fixed arrays.

- [ ] Implement a `CoreMutex`-protected multi-producer/single-consumer fixed
  ring. Admission order is: validate arguments/handle, reserve operation
  record, reserve terminal event, copy command, publish command. Roll back
  reservations in reverse order on failure.

- [ ] Store shutdown completion synchronization inside the service
  implementation, never inside a caller stack frame, preventing the current
  `RunSync()` timeout use-after-return class.

- [ ] Run the unit test with `-pthread` and require
  `OperationMailboxTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/tests/unit/OperationMailboxTest.cpp
  git commit -m "feat(voip): add bounded commands and operations"
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
- Create: `voip/src/core/FakeRuntimeAdapter.hpp`
- Create: `voip/src/core/FakeRuntimeAdapter.cpp`
- Create: `voip/src/core/CoreActor.hpp`
- Create: `voip/src/core/CoreActor.cpp`
- Create: `voip/src/core/VoipResourceGuard.hpp`
- Create: `voip/src/core/VoipResourceGuard.cpp`
- Create: `voip/src/core/VoipRuntime.hpp`
- Create: `voip/src/core/VoipRuntime.cpp`
- Replace: `voip/src/VoipService.cpp`
- Create: `voip/tests/unit/VoipServiceCoreTest.cpp`
- Create: `voip/tests/run_host_tests.sh`

**Interfaces:**

- Consumes: registry, mailbox, operations, event queue, scheduler.
- Produces: functional public service without PJPROJECT, a private runtime
  adapter seam for Plans 3–5, and a build-selectable deterministic fake used
  only by host and Plan 2 integration configurations.

- [ ] Write end-to-end host tests initializing five agents, retrieving handles by config index, dialing seven logical calls, observing two promotions/five FIFO positions, cancelling entries, consuming terminal operation events, rejecting unsupported security, and shutting down twice safely.

- [ ] Add tests proving one agent never owns two promoted calls and that event polling never invokes callbacks.

- [ ] Add pressure/accounting tests proving `GetResourceSnapshot()` reports
  exact available command, operation, event, FIFO, logical-call, and promoted
  capacities after initialization, after admission, after cancellation, and
  after cleanup.  Counters must return to their initialized baseline without a
  diagnostics thread or timer.

- [ ] Add end-to-end trace assertions for initiation, FIFO wait, acceptance,
  rejection, timeout, media hold/resume, finish, terminal publication, and
  immediate stale-handle rejection after cleanup. Verify the terminal event
  remains readable after the handle becomes invalid.

- [ ] Compile the host test before replacing `VoipService.cpp` and confirm the
  link fails on the new service methods rather than on a test syntax error.

- [ ] Define `RuntimeAdapter` methods for account initialization, promote
  outgoing, promote incoming, answer/reject/cancel/hangup/hold, poll native
  events, begin call teardown, and shutdown. Use opaque integer tokens and
  copied notification records only; no PJ type crosses into core headers.

- [ ] Implement `FakeRuntimeAdapter` in `voip/src/core`, not under tests.  It
  records bounded requests and emits scripted copied notifications; it must
  not bypass `CallScheduler` or mutate public snapshots.  Compile it only for
  host tests or `CONFIG_VOIP_SERVICE_FAKE_ADAPTER=y`.  Plan 3 replaces the
  selected adapter with `PjsuaRuntimeAdapter` without changing core headers.

- [ ] Implement always-enabled `VoipResourceGuard` as actor-owned correctness
  counters only.  It has no thread, timer, queue, or dynamic storage and copies
  the public `ResourceSnapshot`; detailed peaks and PJ statistics remain
  outside Plan 2 diagnostics.

- [ ] Implement `VoipRuntime::Step(now_ms)` to drain bounded commands, apply timers, run strict promotion, poll the adapter, update snapshots, and publish events. The Zephyr actor repeatedly calls `Step()`; host tests call it deterministically.

- [ ] Implement `CoreActor` with statically owned thread/stack/completion
  storage.  Its host branch uses `std::thread`; its `__ZEPHYR__` branch uses
  documented kernel thread APIs and a fixed stack configured at build time.
  It repeatedly calls `VoipRuntime::Step()`.  No public header exposes either
  platform's native thread type.

- [ ] Implement public APIs as validation/copy/admission only. They must not
  synchronously wait for operation completion. Implement synchronous
  `Shutdown()` with service-owned completion state and bounded timeout.  A
  timeout leaves the actor and all reachable storage alive in
  `shutting_down`; it never detaches or destroys reachable state.

- [ ] Replace `VoipService.cpp` by placement-constructing `Impl` in the public
  header's aligned storage.  Prove construction/destruction and five repeated
  initialize/shutdown cycles with a host test.  Do not retain the old
  `Backend`, callback, or workqueue compatibility path.  Add
  `static_assert(sizeof(Impl) <= 131072)` and an alignment assertion before the
  placement construction.

- [ ] During shutdown reject new commands, complete or cancel every accepted
  operation exactly once, enqueue `service_stopped` last, and forbid every
  later producer path.  Prove previously queued events remain readable and a
  second `Shutdown()` emits no duplicate stopped event.

- [ ] Create `voip/tests/run_host_tests.sh` with one explicit compile/run entry
  for every Plan 2 unit binary.  Use `-std=c++17 -Wall -Wextra -Werror
  -pthread`, write all output under a unique `/tmp/voip-plan2-host-*`
  directory, stop on the first failure, and remove only that explicit output
  directory on success.

- [ ] Run `voip/tests/run_host_tests.sh` and require every Task 1–7 marker,
  ending with `VoipServiceCoreTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip
  git commit -m "feat(voip): compose bounded multi-agent core service"
  ```

## Task 8: Wire and validate the fake-only Zephyr integration image

**Files:**

- Create: `applications/voip_integration/src/core_service.cpp`
- Create: `applications/voip_integration/core_service.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`
- Create: `voip/tests/unit/NoHeapAfterInitTest.cpp`
- Modify: `voip/tests/run_host_tests.sh`

**Interfaces:**

- Consumes: Task 7 `VoipService`, `CoreActor`, and `FakeRuntimeAdapter`.
- Produces: a Zephyr-selectable PJ-independent service, fixed capacity Kconfig
  contract, QEMU behavior proof, and host proof that steady-state core paths do
  not allocate.

- [ ] Add `NoHeapAfterInitTest.cpp` first.  Override host allocation counters,
  permit construction/`Initialize()`, reset the counter after successful
  initialization, then exercise command admission, two promotions, five FIFO
  entries, event polling/coalescing, cancellation, timeout processing, and
  shutdown.  Require zero allocations between the reset and completed
  shutdown.  Run it against Task 7 and capture the expected RED result before
  changing implementation or build wiring.

- [ ] Add fixed Kconfig constants with compile-time checks: agents 5, logical
  calls 7, promoted calls 2, pending FIFO 5, commands 16, operations 16, and
  events 32. Add `CONFIG_VOIP_SERVICE` and
  `CONFIG_VOIP_SERVICE_FAKE_ADAPTER`; retain `CONFIG_VOIP_PJSUA` for Plan 3 and
  make fake, PJSUA, and the old lower-case backend mutually exclusive.

- [ ] Wire only the focused public/core sources into the VoIP module when
  `CONFIG_VOIP_SERVICE=y`.  Select `FakeRuntimeAdapter.cpp` only when its test
  gate is enabled.  Do not use source globs and do not compile legacy
  `PjVoipBackend.cpp` into the new service target.

- [ ] Implement `core_service.cpp` through the public polling API. Initialize
  five distinct agents and audio sentinels, admit seven mixed logical calls,
  prove two promoted and five FIFO with same-agent exclusion and strict
  head-of-line blocking, consume terminal events, and shut down twice.  Emit
  only this success terminator after all assertions pass:

  ```text
  VOIP CORE SERVICE RESULT: PASSED
  ```

- [ ] Run the complete host suite and require `NoHeapAfterInitTest PASSED`.

- [ ] Build from the workspace parent without inspecting Zephyr source:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d /tmp/voip-plan2-core-service -- \
    -DEXTRA_CONF_FILE=core_service.conf
  ```

- [ ] Run `west build -d /tmp/voip-plan2-core-service -t run` under a bounded
  timeout and require `VOIP CORE SERVICE RESULT: PASSED` before the emulator is
  stopped. Record FLASH/RAM and actor-stack evidence in the task report; do not
  claim target-board qualification.

- [ ] Run `git diff --check`, then commit:

  ```sh
  git add voip applications/voip_integration
  git commit -m "test(voip): validate bounded core service"
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
