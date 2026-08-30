# Claude Review Guide: Completed VoIP Plan 1 and Plan 2

## Purpose

Use this document to perform an independent, evidence-based review of the VoIP work completed in Plan 1 and Plan 2 at repository head `688de098f` on branch `sip_rtp`.

The review must answer two separate questions:

1. Did the implementations satisfy their approved Plan 1 and Plan 2 contracts?
2. Do those implementations form a credible foundation for eventual deployment on the NXP i.MX RT1064 Zephyr target?

Passing QEMU or host tests is evidence, not proof of target-board readiness. Do not claim i.MX RT1064 qualification without a target build and target measurements.

## Reviewer Operating Contract

Claude is a reviewer, not the implementation agent.

- Review the current code and documentation; do not edit files, stage changes, commit, rebase, merge, or push.
- Report findings with exact file and line references.
- Run read-only checks, host tests, and builds when useful. Build output and temporary files must go under an explicit `/tmp/claude-voip-review-*` path.
- Do not inspect, search, index, or modify anything under top-level `zephyr/`.
- Do not use recursive commands that may enter `zephyr/`. Use scoped paths or exclusions such as `rg -g '!zephyr/**'`.
- Treat Zephyr as an external dependency. Use documented commands such as `.venv/bin/west boards`, `.venv/bin/west build`, and build targets/reports.
- If answering a review question genuinely requires Zephyr implementation source, stop that line of investigation and report exactly why the prohibited inspection would be needed.
- Do not count an intentionally deferred Plan 3–6 feature as a Plan 1/2 defect unless Plan 1/2 falsely claims it is complete or the current foundation prevents its later implementation.
- Do not review the Plan 3 implementation plan or any uncommitted Plan 3 documentation as implemented code.
- Do not accept comments, plan checkboxes, pass strings, or acceptance prose as proof by themselves. Trace important claims to code and independently reproducible evidence.

## Review Baseline

### Plan 1

**Plan:** `docs/superpowers/plans/2026-08-27-pjsua-zephyr-port-bounded-arena.md`

**Acceptance record:** `docs/voip/pjsua-zephyr-port.md`

**Implementation interval:** begins after planning commit `f48af7215` and is accepted through `e4a231989`.

**Primary scope:**

- PJSUA-LIB C API link closure for Zephyr
- Explicit PJPROJECT source manifests
- Compile-time capacities of five accounts, seven calls, and twelve conference ports
- Caller-driven `pjsua_handle_events()` with zero PJSUA/PJMEDIA workers
- Dedicated fixed-capacity PJ pool arena
- Arena exhaustion, coalescing, cleanup, and repeated lifecycle validation
- TLS, SRTP, video, and platform audio disabled
- QEMU port evidence only

### Plan 2

**Plan:** `docs/superpowers/plans/2026-08-27-voip-core-service-fifo-scheduler.md`

**Implementation interval:** public-contract work begins at `812369563` and the reviewed/fixed result ends at current head `688de098f`.

**Primary scope:**

- Breaking PJ-independent public service API
- Immutable initialization-time agent registry
- Generation-safe handles
- Polling event queue with guaranteed terminal delivery
- Fixed command mailbox and operation table
- Approved business call state machine
- Two promoted-call slots globally
- At most one promoted call per agent
- One strict five-entry FIFO shared by incoming and outgoing calls
- Deliberate head-of-line blocking
- Actor-owned command processing with deterministic fake adapter
- Ordered, retryable shutdown
- Host no-heap-after-initialization proof
- Fake-only QEMU integration evidence

### Useful history commands

```sh
git log --oneline --reverse f48af7215..e4a231989 -- \
  pjproject/zephyr pjproject/Kconfig applications/voip_integration docs/voip

git log --oneline --reverse e4a231989..688de098f -- \
  voip applications/voip_integration docs/superpowers

git diff --stat f48af7215..688de098f -- \
  pjproject/zephyr pjproject/Kconfig voip applications/voip_integration \
  docs/voip docs/superpowers/specs
```

Do not infer correctness from commit messages. They are only navigation aids.

## Required Reading Order

Read these before forming a verdict:

1. `AGENTS.md`
2. `designIdead.md`
3. `reviews.md`
4. `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`
5. `docs/superpowers/specs/state_machine.uml`
6. Plan 1 and its acceptance record
7. Plan 2
8. Current Plan 1 source and validation profiles
9. Current Plan 2 public headers, core source, tests, and validation profile

Use `voip_stack.md` and the uppercase legacy stack only as migration context. They are not the contract for the replacement lowercase `voip/` architecture.

## Fixed Product and Architecture Constraints

Review against these decisions; do not propose alternatives as if they were missing requirements:

- One to five agents are configured only during service initialization.
- The agent topology, SIP identity, credentials, microphone binding, and speaker binding are immutable until shutdown.
- Each eventual production agent owns one SIP account and independent registration lifecycle.
- At most two calls are promoted globally.
- At most one call is promoted for a given agent.
- Up to five additional incoming/outgoing calls share one strict FIFO.
- A busy agent at the FIFO head blocks later eligible agents; bypass is forbidden.
- Application events use polling, not application callbacks.
- One actor owns mutable domain and eventual PJSUA signaling state.
- PJSUA-LIB C API is required; PJSUA2 is not used.
- PJSUA workers are disabled; the actor drives `pjsua_handle_events()`.
- No C++ or general system-heap allocation is allowed after successful initialization.
- PJ runtime allocations must use the dedicated bounded arena and must not fall back to the general heap.
- Initial signaling is plain SIP over TCP.
- Initial media is plain RTP/RTCP when media is implemented.
- TLS and SRTP retain policy boundaries but are disabled and unused for now.
- Codec selection/priorities are deferred.
- Plan 2 uses a deterministic fake adapter; production account registration, call control, and audio integration are not Plan 2 deliverables.

## Intended Target: NXP i.MX RT1064

The local Zephyr installation exposes the board name `mimxrt1064_evk`. Treat it as the initial build proxy for the intended i.MX RT1064 product target.

Do not assume the EVK's enabled memories, external memory, Ethernet setup, clocks, or peripherals exactly match the final product board. The review must separate:

- architecture correctness;
- successful `mimxrt1064_evk` compilation;
- EVK runtime qualification;
- final custom-board qualification.

### Target constraints Claude must evaluate

- Total static image RAM, not only the PJ arena's measured live usage
- The configured 2 MiB QEMU PJ arena versus the actual approved target memory budget
- `VoipService`'s 131072-byte placement storage
- PJSUA global/static records, PJ arena metadata, Zephyr kernel objects, libc arena, system heap, network packet/context buffers, main stack, and 4096-byte core actor stack
- Whether Plan 1 and Plan 2 have only been measured separately and what their combined production footprint may require
- Linker placement of the arena and whether it assumes internal RAM, external RAM, or a new target linker/Devicetree policy
- Cortex-M7 alignment and cache coherency expectations for arena memory and future DMA-backed audio buffers
- Stack high-water evidence for the real core actor once it owns PJSUA polling, not only Plan 1's QEMU main-thread surrogate
- Zephyr TCP socket/context limits and network-buffer pressure under five accounts and future seven signaling records
- Removal or isolation of host-only facilities such as `std::thread`, `std::mutex`, exceptions, or POSIX behavior from `__ZEPHYR__` paths
- Bounded shutdown and callback lifetime on a single-core preemptive embedded target
- Startup-order dependencies for network readiness, clocks, entropy, and application-owned audio objects
- Whether eventual microphone/speaker DMA/cache handling can remain behind `PcmSource`/`PcmSink` without changing the Plan 2 public contract

Do not invent exact target RAM, flash, cache, or external-memory values. Obtain them from an approved product hardware source or target build output and identify the source in the report.

## Review Method

For each requirement:

1. Identify the owning component.
2. Trace the public input to the internal owner and final cleanup.
3. Check success, capacity exhaustion, invalid input, timeout, and shutdown paths.
4. Check every pointer's owner and lifetime.
5. Check every mutable state transition's owning thread/lock.
6. Locate a test that would fail if the implementation were wrong.
7. Run the smallest relevant test or build.
8. Report missing evidence separately from demonstrated defects.

Prefer adversarial review: stale handles, full rings, duplicate callbacks, delayed shutdown, initialization rollback, counter wraparound, head-of-line blocking, and partial native failure.

## Plan 1 Review Checklist

### 1. PJSUA source and configuration boundary

- [ ] `CONFIG_PJSUA` selects the complete required source closure without globs.
- [ ] PJSIP-UA owns INVITE/registration objects exactly once; source families do not create duplicate objects.
- [ ] PJSUA2 and sample applications remain outside the selected build.
- [ ] Compile definitions actually produce `PJSUA_MAX_ACC == 5`, `PJSUA_MAX_CALLS == 7`, and `PJSUA_MAX_CONF_PORTS == 12`.
- [ ] Kconfig ranges prevent silently building incompatible capacities.
- [ ] TCP signaling is available without accidentally enabling TLS.
- [ ] SRTP, TLS, video, and unwanted host audio backends are truly compiled out in the accepted profile.
- [ ] STUN, TURN, ICE, and UPnP may be required in the PJSUA link closure but remain disabled at runtime as documented.

Review at minimum:

- `pjproject/Kconfig`
- `pjproject/zephyr/CMakeLists.txt`
- `pjproject/zephyr/sources.cmake`
- PJPROJECT configuration headers changed by Plan 1
- `applications/voip_integration/pjsua_link.conf`
- `applications/voip_integration/pjsua_capacity.conf`

### 2. Fixed PJ arena correctness

- [ ] The arena is statically reserved, correctly aligned, and installed before `pjsua_create()`.
- [ ] All allocator metadata stays inside fixed storage.
- [ ] Size arithmetic rejects zero and overflow safely.
- [ ] Allocation splitting preserves aligned payloads and valid forward/backward boundary tags.
- [ ] Free validates exact payload starts and safely rejects interior, foreign, duplicate, and corrupted pointers.
- [ ] Forward and backward coalescing update neighbor metadata correctly.
- [ ] Counters cannot underflow or claim cleanup while live blocks remain.
- [ ] `reset()` refuses live allocations and returns a completely reusable arena only after cleanup.
- [ ] The PJ pool failure callback does not abort, throw, allocate, or fall back to another heap.
- [ ] Locking is valid for the contexts in which PJ may allocate/free and does not hold a spinlock across blocking work.
- [ ] Statistics are internally consistent and do not expose torn values.
- [ ] Repeated install/reset/destroy behavior is deliberate and tested.

Review at minimum:

- `pjproject/zephyr/pj_zephyr_pool_arena.c`
- `pjproject/zephyr/include/pj_zephyr_pool_arena.h`
- `applications/voip_integration/src/pjsua_arena_test.c`
- arena-related sections of `pjsua_link.c` and `pjsua_capacity.c`

### 3. Actor-driven PJSUA lifecycle evidence

- [ ] Both PJSUA and PJMEDIA worker counts are zero.
- [ ] Progress depends on explicit `pjsua_handle_events()` calls.
- [ ] Callback-affinity tests distinguish PJSUA workers from unrelated Zephyr/analyzer threads.
- [ ] Initialization failures destroy only acquired native stages.
- [ ] Destruction drains/cleans PJSUA and returns the arena to zero live/used bytes.
- [ ] Five lifecycle and 100-cycle tests are meaningful, bounded, and cannot print PASS before completing assertions.
- [ ] Seven native call records are held simultaneously and the eighth is really rejected with SIP 486.
- [ ] Plan 1 does not falsely claim product scheduling, registration, media routing, or target-board qualification.

### 4. Plan 1 evidence quality

Reconcile current output against the acceptance record. Investigate unexplained differences.

The record currently reports QEMU static RAM around 3.5-3.7 MiB for accepted profiles and a 2 MiB configured arena. This is a prominent i.MX RT1064 portability risk, not target qualification.

Required markers:

```text
PJSUA LINK RESULT: PASSED (5 lifecycles, arena clean)
PJSUA ARENA RESULT: PASSED (exhaustion, coalescing, 100 cycles)
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
```

An emulator timeout code of 124 is acceptable only when the expected final marker appeared first and the emulator was intentionally idle afterward.

## Plan 2 Review Checklist

### 1. Public contract and ownership

- [ ] Public headers expose no PJPROJECT type.
- [ ] Legacy constructor/API removal is real, not only hidden by overload resolution.
- [ ] Public handles, events, snapshots, and operations are copied fixed-size records.
- [ ] `Initialize()` validates one to five agents before changing live state.
- [ ] SIP strings and credentials are bounded, copied, NUL-terminated, and erased on rollback/shutdown.
- [ ] Audio source/sink pointers are borrowed with a clear lifetime and duplicate bindings are rejected.
- [ ] TLS/SRTP policy values are retained in the API but rejected as unsupported.
- [ ] Unsupported PCM formats are rejected before partial initialization.
- [ ] `VoipService` placement construction/destruction is aligned and bounded; the 128 KiB budget is measured for the target rather than assumed harmless.

Review at minimum:

- `voip/include/voip/VoipService.hpp`
- `voip/include/voip/VoipTypes.hpp`
- `voip/include/voip/VoipEvents.hpp`
- `voip/include/voip/PcmAudio.hpp`
- `voip/src/VoipService.cpp`
- `voip/src/core/AgentContext.hpp`
- `voip/src/core/AgentRegistry.*`
- `voip/tests/unit/PublicContractTest.cpp`
- `voip/tests/unit/AgentRegistryTest.cpp`

### 2. Handles and bounded storage

- [ ] Agent and call handles use slot plus nonzero generation.
- [ ] Release and lifecycle reset invalidate stale handles.
- [ ] Generation wrap cannot make an immediately stale handle valid.
- [ ] Fixed pools never construct outside their storage or destroy a live object twice.
- [ ] Capacities agree across code, Kconfig, tests, resource snapshots, and the approved architecture.
- [ ] No vector, map, string, future, promise, or hidden allocator appears on post-initialization paths.

Review at minimum:

- `voip/src/HandlePool.hpp`
- `voip/src/core/AgentRegistry.*`
- `voip/src/core/CallContext.hpp`
- `voip/src/core/CallScheduler.*`
- handle and scheduler unit tests

### 3. Business state machine

- [ ] Public states are exactly `idle`, `initiated`, `established`, `hold`, and `terminated`.
- [ ] Every edge in `state_machine.uml` is implemented and invalid edges are rejected.
- [ ] `hold(waiting)` is distinct from negotiated `hold(media)`.
- [ ] Private queue/signaling/teardown phases are not leaked or duplicated into a second public state owner.
- [ ] Terminal snapshots are published before immediate handle invalidation and slot return to `idle`.
- [ ] Stale handles fail after cleanup while copied terminal events remain pollable.

### 4. Strict scheduler behavior

- [ ] No more than two calls are promoted globally.
- [ ] No more than one call is promoted per agent.
- [ ] Incoming and outgoing calls use the same five-entry FIFO.
- [ ] FIFO ordering is strict.
- [ ] An ineligible same-agent head blocks later eligible entries even when a global slot is free.
- [ ] Queued cancellation/timeout removes the exact entry without corrupting order.
- [ ] Promotion waits for prior teardown completion rather than only a terminal business state.
- [ ] Two promoted plus five queued consumes all seven logical slots; the next admission fails deterministically.
- [ ] Queue and answer deadlines handle time arithmetic/wrap safely.

Pay special attention to the head-of-line witness in both `CallSchedulerTest.cpp` and `applications/voip_integration/src/core_service.cpp`; ensure the test would fail if bypass occurred.

### 5. Events, commands, and operations

- [ ] Event capacity is 32 with one permanently protected `service_stopped` slot.
- [ ] Every accepted operation reserves terminal-event capacity before admission and completes exactly once.
- [ ] Registration failures, call terminal events, timeouts, incoming admission, operation terminals, and service stop have the intended guarantee.
- [ ] Intermediate snapshots coalesce only for the same type and handle generation.
- [ ] Sequence numbers remain monotonic across coalescing and do not expose zero.
- [ ] Semaphore/signal state cannot lose wakeups or leave stale wakeups across lifecycle reset.
- [ ] Reservation destruction/cancellation cannot race queue destruction.
- [ ] Command records never point at caller stack completion objects.
- [ ] Mailbox/operation exhaustion returns the documented error without leaking reservations.
- [ ] Concurrent producers and the single polling consumer follow the documented locking contract.

Review at minimum:

- `voip/src/core/VoipEventQueue.*`
- `voip/src/core/CommandMailbox.*`
- `voip/src/core/OperationTable.*`
- `voip/src/core/CoreSynchronization.hpp`
- corresponding unit and regression tests

### 6. Actor, adapter seam, and shutdown

- [ ] Public threads only validate/copy/admit bounded records; actor processing owns domain mutations intended to be actor-owned.
- [ ] Host `std::thread` code is excluded from Zephyr builds; the Zephyr path uses fixed `k_thread` storage and a fixed 4096-byte stack.
- [ ] Actor start/stop, pause seams, and destructor behavior do not race or self-join.
- [ ] Shutdown is idempotent and concurrent callers observe one consistent completion.
- [ ] Shutdown timeout preserves all callback-reachable/native storage and a later call can complete teardown.
- [ ] New admission stops before queued/native teardown begins.
- [ ] Operations complete, calls release, adapter shutdown completes, credentials erase, handles invalidate, and `service_stopped` publishes last.
- [ ] No event can publish after `service_stopped`.
- [ ] Runtime locks, shutdown coordination, event locks, and actor join order have no inversion or wait-while-holding deadlock.
- [ ] Adapter failures do not release promoted leases or native tokens prematurely.

Known integration seam to classify carefully: current Plan 2 calls the fake adapter's per-account initialization before the production actor starts and immediately projects fake registration success. This is acceptable only as a clearly contained fake-stage behavior. It is incompatible with actor-owned PJSUA initialization and must not be mistaken for a production-ready adapter contract. Report it as a Plan 3 integration risk unless it already violates a Plan 2 guarantee or introduces a current correctness bug.

### 7. No-heap proof quality

- [ ] The test observes allocations from all relevant C++ allocation entry points used by the core.
- [ ] Allocation tracking begins after successful initialization and covers dial, queueing, events, cancellation, timeout, shutdown, and repeated lifecycle paths.
- [ ] The proof does not miss allocations in another thread due to unsynchronized counters.
- [ ] Placement construction is not incorrectly counted as heap allocation.
- [ ] Host proof is not presented as proof that PJPROJECT or Zephyr networking allocates nothing.
- [ ] Target builds remain exception/RTTI-policy compatible and do not introduce hidden runtime allocation paths.

### 8. Plan 2 evidence quality

Required host markers include all unit suites and:

```text
VoipServiceCoreTest PASSED
NoHeapAfterInitTest PASSED
ShutdownMailboxRegressionTest PASSED
```

Required QEMU marker:

```text
VOIP CORE SERVICE RESULT: PASSED
```

Verify that the QEMU scenario actually proves two promoted calls, five queued calls, head-of-line blocking, timeout cleanup, and idempotent shutdown. A pass marker alone is insufficient.

## Cross-Plan Integration Review

Plan 1 and Plan 2 were deliberately implemented independently. Review whether their boundaries can be composed without invalidating either proof.

- [ ] The Plan 2 public API and core headers remain PJ-independent when a PJSUA adapter is added.
- [ ] The Plan 2 actor can become the sole caller of the Plan 1 PJSUA event pump.
- [ ] PJSUA initialization/teardown can be moved entirely onto the actor without application-thread mutation or deadlock.
- [ ] Plan 1's five account/seven call capacities match Plan 2's five agent/seven logical call capacities.
- [ ] Seven native call records are sufficient for two promoted calls plus five queued incoming calls, while queued outgoing calls allocate no native call.
- [ ] The fixed event/command/operation capacities can absorb worst-case PJSUA callback bursts or have an explicit backpressure/failure policy.
- [ ] Combined C++ fixed storage plus PJ arena plus PJSUA globals plus Zephyr networking is budgeted, not inferred from separate tests.
- [ ] Shutdown ordering can unregister accounts and drain PJSUA callbacks before registry credentials/context storage are erased.
- [ ] PJSUA callback user-data can point only to stable preallocated contexts that outlive quiescence.
- [ ] Future per-agent audio binding can resolve `AgentHandle -> AgentContext -> audio` without storing devices in PJSUA account contexts.

Any boundary that requires redesign should be reported before Plan 3 implementation begins.

## i.MX RT1064 Readiness Review

### Permitted discovery

```sh
.venv/bin/west boards | rg '^mimxrt1064_evk$'
```

Do not read the board implementation under `zephyr/`.

### Build-only probes

Run these as review evidence. Do not change source/config merely to make them pass.

Plan 2 core:

```sh
.venv/bin/west build -p always -b mimxrt1064_evk \
  applications/voip_integration \
  -d /tmp/claude-voip-review-rt1064-core -- \
  -DEXTRA_CONF_FILE=core_service.conf
```

Plan 1 PJSUA link profile:

```sh
.venv/bin/west build -p always -b mimxrt1064_evk \
  applications/voip_integration \
  -d /tmp/claude-voip-review-rt1064-pjsua -- \
  -DEXTRA_CONF_FILE=pjsua_link.conf
```

If supported by the generated build, collect documented reports:

```sh
.venv/bin/west build -d /tmp/claude-voip-review-rt1064-core -t ram_report
.venv/bin/west build -d /tmp/claude-voip-review-rt1064-core -t rom_report
.venv/bin/west build -d /tmp/claude-voip-review-rt1064-pjsua -t ram_report
.venv/bin/west build -d /tmp/claude-voip-review-rt1064-pjsua -t rom_report
```

For every build, record:

- exact command and commit;
- success/failure and first root-cause error;
- FLASH and RAM totals from build output;
- largest static symbols from reports/map output;
- configured heap/libc arena, main stack, actor stack, network buffers, PJ arena, and service storage;
- whether the image assumes memory not approved for the final product;
- warnings that could become target runtime defects.

A target linker overflow or unsupported network configuration is useful evidence. Report it; do not hide it by shrinking capacities or enabling external memory without product approval.

### Hardware evidence still required after a successful build

- Actor and main-thread stack high-water under worst-case signaling
- PJ arena peak, largest free block, fragmentation, and allocation failures
- System/libc heap high-water and proof of no post-init fallback
- TCP socket/context and network-buffer peaks
- Five-account registration and seven-record signaling stress
- Repeated boot/init/shutdown cycles
- Cache/alignment behavior of any externally placed arena
- CPU load and event-loop latency
- Ethernet disconnect/reconnect behavior
- Watchdog compatibility during PJSUA polling and teardown
- Future audio DMA/cache coherency and per-agent routing qualification

Until these exist, the correct status is “architecture/compile reviewed; i.MX RT1064 runtime qualification pending.”

## Reproduction Commands

### Plan 2 host suite

```sh
voip/tests/run_host_tests.sh
```

Run it at least twice when investigating lifecycle or race behavior.

### Plan 2 QEMU integration

```sh
.venv/bin/west build -p always -b mps2/an385 \
  applications/voip_integration \
  -d /tmp/claude-voip-review-core-qemu -- \
  -DEXTRA_CONF_FILE=core_service.conf

timeout 30s .venv/bin/west build \
  -d /tmp/claude-voip-review-core-qemu -t run
```

Require `VOIP CORE SERVICE RESULT: PASSED` before accepting an intentional timeout.

### Plan 1 QEMU profiles

```sh
.venv/bin/west build -p always -b mps2/an385 \
  applications/voip_integration \
  -d /tmp/claude-voip-review-pjsua-link -- \
  -DEXTRA_CONF_FILE=pjsua_link.conf

.venv/bin/west build -p always -b mps2/an385 \
  applications/voip_integration \
  -d /tmp/claude-voip-review-pjsua-arena -- \
  -DEXTRA_CONF_FILE=pjsua_arena.conf

.venv/bin/west build -p always -b mps2/an385 \
  applications/voip_integration \
  -d /tmp/claude-voip-review-pjsua-capacity -- \
  -DEXTRA_CONF_FILE=pjsua_capacity.conf

timeout 30s .venv/bin/west build \
  -d /tmp/claude-voip-review-pjsua-link -t run
timeout 30s .venv/bin/west build \
  -d /tmp/claude-voip-review-pjsua-arena -t run
timeout 40s .venv/bin/west build \
  -d /tmp/claude-voip-review-pjsua-capacity -t run
```

## Finding Severity

- **P0 — Critical:** memory corruption, credential disclosure, unsafe free/use-after-free, unbounded allocation fallback, or a defect capable of invalidating the embedded system.
- **P1 — High:** violated fixed product invariant, deadlock, lost guaranteed event, stale-handle acceptance, wrong FIFO/promotion behavior, unsafe shutdown, false no-heap claim, or Plan 1/2 exit criterion not met.
- **P2 — Medium:** i.MX RT1064 portability blocker, incomplete failure handling, materially weak test that permits a serious regression, capacity/configuration mismatch, or undocumented integration redesign required before Plan 3.
- **P3 — Low:** maintainability or diagnostic weakness with a concrete future correctness/qualification cost.

Do not report style preferences, naming tastes, speculative optimizations, or already-deferred features as findings.

## Required Review Report Format

```markdown
# Plan 1 and Plan 2 Independent Review

## Verdict

One of: PASS / PASS WITH RISKS / CHANGES REQUIRED / BLOCKED

Two to five sentences separating:
- Plan 1 correctness
- Plan 2 correctness
- Plan 1+2 integration readiness
- i.MX RT1064 readiness

## Findings

### [P1] Short actionable title

- Location: `path/to/file:line`
- Requirement: exact plan/spec/invariant being violated
- Evidence: code path, test output, or build output
- Failure scenario: smallest reproducible sequence
- Consequence: current behavior and i.MX RT1064 impact
- Recommended correction: bounded change, without editing it
- Validation: exact test/build that should fail before and pass after

Repeat in descending severity. Write "No findings" if none.

## Plan 1 Exit-Criteria Matrix

| Criterion | PASS/FAIL/UNPROVEN | Code evidence | Test/build evidence |
| --- | --- | --- | --- |

## Plan 2 Exit-Criteria Matrix

| Criterion | PASS/FAIL/UNPROVEN | Code evidence | Test/build evidence |
| --- | --- | --- | --- |

## Cross-Plan Integration Risks

| Risk | Severity | Evidence | Required before Plan 3? |
| --- | --- | --- | --- |

## i.MX RT1064 Readiness

- Board build status:
- Static RAM/FLASH evidence:
- PJ arena budget status:
- Service/actor/network budget status:
- External-memory/cache assumptions:
- Missing hardware measurements:
- Readiness classification:

## Validation Performed

| Command | Result | Relevant marker/measurement |
| --- | --- | --- |

## Deferred Scope Confirmed

List items correctly deferred to Plans 3–6 and therefore not counted as defects.

## Positive Evidence

List only unusually strong design/test evidence that materially increases confidence.
```

## Final Reviewer Rules

- Lead with findings, ordered by severity.
- Every finding needs exact evidence and a plausible failure scenario.
- Mark an item `UNPROVEN` when evidence is absent; do not convert uncertainty into PASS.
- Distinguish a current defect from a future integration risk.
- Distinguish QEMU proof, target compilation, EVK runtime proof, and final-board qualification.
- Do not approve target deployment based on host/QEMU evidence alone.
- Do not modify the repository while performing this review.
