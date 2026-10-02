# PJSUA Integration Corrections and Media Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or, if selected by the user, superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the remaining call/media contract violations, then complete the two isolated application audio paths required by Plan 5.

**Architecture:** Preserve the actor, scheduler, seven native contexts, and immutable agent bindings. Defer incoming native media until scheduler promotion; report failed media controls independently of call termination. Add two stable audio bridges and an explicitly driven conference clock, with ownership retained through callback and conference-operation quiescence.

**Tech Stack:** C++17, the checked-in PJPROJECT port, PJSUA/PJMEDIA C APIs, fixed storage, MPS2/AN385 QEMU, host tests, scripted SIP and G.711 RTP/RTCP peers.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md` and `docs/superpowers/specs/state_machine.uml`.

**Status:** Proposed execution plan, based on local `sip_rtp` at `f25dec78ab82041582a93b26887575937105d00d`, reviewed 2026-10-01. No implementation changes are included in this audit. This plan supplies corrections and an updated Plan 5 sequence; it does not claim Plan 5 acceptance or replace the approved product requirements.

## Global Constraints

- Never inspect, search, index, or modify the external top-level `zephyr/` source tree. Use documented build commands and their output. Project-owned `voip/zephyr/` and `pjproject/zephyr/` are module integration files, not that dependency tree.
- Keep five agents, seven logical/native call contexts, two promoted leases, at most one promoted call per agent, five FIFO entries, sixteen commands, sixteen operations, and thirty-two events.
- Keep `PJSUA_MAX_ACC=5`, `PJSUA_MAX_CALLS=7`, and `PJSUA_MAX_CONF_PORTS=12`.
- Keep one actor owning PJSUA/signaling mutations; PJSUA and PJMEDIA worker counts remain zero. No callbacks publish directly to the application.
- Keep strict mixed incoming/outgoing FIFO, admission-time queue timers, promotion-time answer timers, guaranteed terminal events, and generation invalidation.
- Queued calls own no RTP sockets, codec instances, conference ports, or application audio bridges. A queued incoming call may retain its SIP dialog and copied SDP.
- Keep `pjsua_set_no_snd_dev()`. Use plain TCP signaling and UDP RTP/RTCP. TLS, SRTP, PJSUA2, codec-priority policy, and hardware audio remain outside this increment.
- No C++ or general system-heap allocation after successful initialization. PJ allocations use only the fixed arena, with no fallback. QEMU arena capacity remains 2,097,152 bytes; normal peak must stay at or below 75%, injected failures below 90%.
- Retain signaling/native ownership and promoted leases until media, callbacks, conference operations, and future security cleanup are quiescent. A shutdown timeout preserves reachable storage.
- Preserve the existing PCM public signatures and valid configurations, including the current 8 kHz/80-sample test profile. The new end-to-end audio profile uses 8 kHz/160-sample mono signed-16 PCM.
- Preserve the legacy uppercase stack and lowercase compatibility code until the separately gated Plan 6 migration.
- RT1064 builds provide compile/link and memory evidence only. They do not establish hardware, external-network, audio-device, or production RAM acceptance.

## Audit result and evidence

### Current status

| Track | Evidence and status |
| --- | --- |
| Earlier PJLIB/PJSIP/PJMEDIA and facade integration | Historical QEMU validation exists through PJSIP Phase 11, PJMEDIA Phase 12, and facade Phase 9. These are a different, mostly single-call path; their audio results do not validate the new PJSUA service. |
| PJSUA Plans 1–3 | Implemented; port/arena and five-account acceptance records exist in `docs/voip/`. Their entire matrix was not rerun during this audit. |
| PJSUA Plan 4 | Implemented, including final review repairs in current HEAD. Fresh host/component/multi-call checks pass, but the queued-media resource invariant and failed media-control completion still need correction. |
| PJSUA Plan 5 | `PjsuaAudioPort`, `PjsuaMediaBridge`, and `PjsuaMediaManager` are absent. PCM interfaces and basic format validation already exist. The old plan needs the clock, native-media deferral, build wiring, and lifecycle details below. |
| PJSUA Plan 6 | Pending. `VoipResourceGuard` exists as a snapshot holder, not the full invariant-enforcement implementation described by Plan 6. Rebase that task rather than recreating its files. |

### Findings requiring work

1. **High: queued incoming calls already allocate native media transport.** In `pjproject/pjsip/src/pjsua-lib/pjsua_call.c:2120`, an SDP-bearing incoming INVITE invokes `pjsua_media_channel_init()` before `on_incoming_call` at line 2303. `pjsua_media.c:2205` creates UDP transport on this path. In the fresh multi-call run, native calls 2–6 open RTP sockets on ports 4004–4012 while both promoted leases are occupied. Additionally, `PjsuaCallManager::AdmitIncoming()` passes a non-null default setting to `call_answer2(...,180,...)`; `pjsua_call_answer2()` uses `opt_inited` to initialize media even for an offerless call. Merely delaying bridge acquisition, or changing `aud_cnt` after the callback, cannot satisfy the no-RTP-while-queued rule.

2. **High: rejected hold/resume can remain pending indefinitely.** `PjsuaCallbackRouter::Attach()` installs call-state and media-state callbacks, but no transaction-result callback. `PjsuaCallManager::OnCallMediaState()` completes controls only on selected media callbacks. Native `pjsua_call_on_media_update()` returns without that callback after failed SDP negotiation on a confirmed dialog. Current HEAD permits hangup to cancel the unresolved operation, as its acceptance record explicitly explains; that escape does not satisfy the required failure completion while preserving the existing call. This audit confirmed the source path and existing regression coverage; it did not add or run a new rejected-re-INVITE test.

3. **High, Plan 5 omission: no conference clock owner.** `PjsuaRuntime.cpp:50` discards the master port returned by `set_no_sound()`. The old Plan 5 only adds conference connections. The checked-in `pjmedia/conference.h:329` explains that the bridge processes frames when its master port is driven. Preserve no-sound-device mode and explicitly supply timing. This also follows the [PJSUA media API contract](https://docs.pjsip.org/en/2.17/api/generated/pjsip/group/group__PJSUA__LIB__MEDIA.html); the checked-in headers remain the version authority.

4. **Plan 5 integration and lifetime gaps.** The old file lists omit native API seams, callback router, runtime composition, module source registration, and core bridge resource accounting. `media_quiescent` currently reflects native status, not custom-port callbacks; `available_media_bridges` currently aliases available promoted leases. Both need real bridge ownership once audio exists. Port rollback must obey the same quiescence rules as normal teardown. Negotiated `media_dir` is local encode/decode direction; do not invert it as if it were raw remote SDP.

5. **Documentation drift.** The old integration roadmap still says “Begin with Phase 0”; the Plan 4 handoff describes a pre-implementation Git reconciliation that current local history has passed. Old Plan 5 Task 1 would recreate existing PCM declarations and refers to a `frame_time_ms` field absent from `ServiceConfig`. Treat those as historical records, not instructions to redo completed work.

### Fresh verification performed for this audit

- `bash voip/tests/run_host_tests.sh`: exit 0, all eleven existing markers.
- Fresh `pjsua_call_component.conf` build: exit 0; FLASH 90,356 bytes, RAM 1,421,872 bytes. QEMU emitted the five individual markers and `PJSUA PLAN 4 COMPONENT RESULT: PASSED`; the 20-second wrapper expired after PASS, with no whole-log fault signature.
- Fresh `multi_call_scheduler.conf` build: exit 0; FLASH 633,220 bytes, RAM 3,924,596 bytes. QEMU emitted `VOIP MULTI CALL RESULT: PASSED (2 promoted, 5 FIFO, 5 lifecycles)`. The 70-second wrapper exited 124 after PASS; the verdict command exited 0 with no whole-log fault signature. All five lifecycles reported zero general-heap allocations and arena usage/live blocks returning to zero. Maximum lifetime arena peak was 600,800 bytes (28.65%); peer minimum unused stack was 3,968 bytes. These checks do not assert absence of queued RTP transports, which the same log disproves.
- Fresh build/run logs use `/tmp/voip-audit-20261001-component-*` and `/tmp/voip-audit-20261001-multicall-*`. These are temporary evidence, not durable acceptance artifacts.
- No fresh RT1064, real PBX, hardware audio, full Plan 1–3 matrix, or 100-lifecycle qualification was performed. Historical acceptance remains explicitly historical.

Fresh log SHA-256 values:

| Log suffix | SHA-256 |
| --- | --- |
| `component-build.log` | `40796e545adb99d59c80420f4cc368d51422d8e3a7000225caf6e346a534f2b2` |
| `component-run.log` | `294a9ca265539b2c1c1e5943650523b0035b625ef54bb4e2c67ee1c77c5d0308` |
| `multicall-build.log` | `6c5c0bec944369b002bc9128a803dd4c74333729b5c5cbbeaa5f6599a427291e` |
| `multicall-run.log` | `3fa40a6c5d8ffbc379bd0e686f46870695505f3cd7ffb0fbff56447eb365e0f4` |

## Review Focus

Each failure class below has assertions assigned to a task; these are the gaps the existing happy-path verdicts do not settle.

1. SDP-bearing and offerless incoming INVITEs while both leases are occupied: zero media transport creation before promotion (Task 1).
2. Rejected, timed-out, duplicate, stale, or unusable-SDP re-INVITEs: one operation result, preserved previous state, no stuck control (Task 2).
3. Clock lateness, differing frame sizes, and partial failed source writes: bounded work and full-frame silence without memory overrun (Tasks 3–4).
4. Remote direction changes and native conference-slot replacement: local-direction mapping and no stale/cross-agent connection (Tasks 5–7).
5. Partial attach failures or shutdown with callbacks/operations still reachable: no reuse, flush, terminal release, or arena destruction too early (Tasks 5–6).

## Execution order and common verification

Execute Tasks 1 and 2 as the corrective milestone. Then execute Tasks 3–8 as the updated Plan 5. Task 9 records acceptance and the next Plan 6 boundary. Commit each independently tested task; do not merge, push, remove old code, or claim product qualification as part of this plan's creation.

At execution time, use an isolated worktree based on the reviewed revision, preserve unrelated work, and set `audit_source` to that worktree's absolute path. The commands below use an absolute source to avoid the historical `.worktrees/pjsua-plan4` dependency:

```sh
export PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH
export CCACHE_DISABLE=1
audit_source=/home/pitpapan/zephyrproject
bash "$audit_source/voip/tests/run_host_tests.sh"
west build -p always -b mps2/an385 -s "$audit_source/applications/voip_integration" \
  -d /tmp/voip-corrections-component -- -DEXTRA_CONF_FILE=pjsua_call_component.conf
west build -p always -b mps2/an385 -s "$audit_source/applications/voip_integration" \
  -d /tmp/voip-corrections-multicall -- -DEXTRA_CONF_FILE=multi_call_scheduler.conf
```

Run configured profiles using `timeout 20s west build -d /tmp/voip-corrections-component -t run` or `timeout 70s west build -d /tmp/voip-corrections-multicall -t run`, saving complete logs. A timeout exit 124 is acceptable only after the exact expected PASS marker and a clean whole-log scan. Require all relevant assertions and cleanup counters, not just a banner. Scan for MPU/Usage/Bus/Hard/MemManage faults, access violations, invalid EXC_RETURN, fatal/assert/panic/oops/halt, `abort()`, and scenario `FAIL` markers, including after PASS. Other exit statuses must be explained or fail the gate.

For Tasks 3–7 create the component profile named `pjsua_media_component.conf`; build it by substituting that overlay and `/tmp/voip-media-component` in the component command. Require `PJSUA PLAN 5 COMPONENT RESULT: PASSED`. Use static/reused test fixtures; do not enlarge existing product limits or hide test stack overflow by relying on an early marker.

## Task 1: Defer incoming native media until promotion

**Files:** Modify `pjproject/pjsip/include/pjsua-lib/pjsua.h`, `pjproject/pjsip/include/pjsua-lib/pjsua_internal.h`, `pjproject/pjsip/src/pjsua-lib/pjsua_core.c`, `pjproject/pjsip/src/pjsua-lib/pjsua_call.c`; modify `voip/src/pjsua/PjsuaApi.hpp/.cpp`, `PjsuaRuntime.cpp`, `PjsuaCallManager.cpp`, `PjsuaCallContext.hpp`; extend `voip/tests/pjsua/FakePjsuaApi.hpp`, `PjsuaIncomingAdmissionTest.cpp`, `PjsuaOutgoingPromotionTest.cpp`; extend `applications/voip_integration/src/multi_call_scheduler.cpp` and `test_support/ScriptedSipPeer.hpp/.cpp`.

**Interfaces:** Proposed opt-in native `pjsua_config::defer_incoming_media` (default false) and `pj_status_t pjsua_call_prepare_media(pjsua_call_id)`. Add `PjsuaApi::call_prepare_media` with that exact signature. These are proposed port extensions, not existing upstream APIs. Existing `AdmitIncoming(...)` and `PromoteIncoming(...)` remain the service boundary; policy stays in the scheduler.

- [ ] Add real tests `queued_offered_invite_has_no_media_transport` and `queued_offerless_invite_has_no_media_transport`. Extend the existing `pjsua_init` test wrapper to record per-native-call media-transport creation transitions while forwarding production callbacks. With two promoted calls, enqueue five incoming calls: each receives 180, transport-creation count does not increase, and there is no custom conference port or codec for a queued call. Observe counts from the actor through copied test records, not unsynchronized PJSUA calls from the polling thread.
- [ ] Run the multi-call profile and capture the expected RED: current offered INVITEs create transports before admission; current offerless admission can initialize them through non-null settings. Add fake assertions that admission sends 180 without forcing media settings, and promotion prepares media once.
- [ ] Implement opt-in deferral before the incoming `pjsua_media_channel_init()` path. Retain a pool-owned clone of the remote offer and the SIP dialog; never retain receive-buffer pointers. Keep SIP/SDP validation and provisional signaling functional. Do not represent a deliberately deferred call as an in-flight asynchronous transport creation, which would postpone 180 itself.
- [ ] Implement idempotent `pjsua_call_prepare_media()` using the existing native media initialization/completion paths and the retained offer; handle offerless calls with the appropriate local-offer path. Reject terminal/invalid IDs, propagate initialization failure, and clear deferred state on CANCEL/reject/destroy. Default-disabled behavior must preserve existing non-service PJSUA profiles.
- [ ] Enable deferral only in the selected service runtime. In `AdmitIncoming`, send an SDP-free 180 without initializing call settings. In `PromoteIncoming`, prepare media only after the scheduler owns the lease; preserve answer-on-promotion order. Guard `Answer()` against bypassing promotion. Outgoing media remains created only by promoted `call_make_call()`.
- [ ] Run offered/offerless answer, CANCEL-before-promotion, queued reject/480 timeout, eighth-call 486, malformed SDP, arena exhaustion during promotion, and recycled native-ID tests. Assert no leaks, one terminal event, strict FIFO, and successful delayed offer/answer. Re-run the component and multi-call profiles.
- [ ] Record the new native extension in `docs/voip/pjsua-zephyr-port.md` and commit the scoped correction as `fix(voip): defer queued incoming native media`.

**Design boundary:** This opt-in change is necessary to meet the existing no-queued-RTP requirement. A wrapper around `on_create_media_transport` alone runs after native transport creation and cannot prove the invariant. Allowing sockets for queued calls is an alternative architectural change and is not selected here. If the port cannot retain correct SIP/SDP behavior with this bounded deferral, stop this milestone and present that concrete incompatibility; do not silently weaken the requirement.

## Task 2: Complete failed hold and resume operations

**Files:** Modify `voip/src/pjsua/PjsuaCallbackRouter.hpp/.cpp`, `PjsuaCallManager.hpp/.cpp`, `PjsuaCallContext.hpp`, `PjsuaCallRecords.hpp`; extend `voip/tests/pjsua/PjsuaCallControlTest.cpp`, `PjsuaCallStateTest.cpp`, `voip/tests/unit/VoipServiceCoreTest.cpp`; extend `applications/voip_integration/test_support/ScriptedSipPeer.hpp/.cpp` and `src/multi_call_scheduler.cpp`. For negotiation failures not exposed through existing callbacks, modify the native callback declaration in `pjproject/pjsip/include/pjsua-lib/pjsua.h` and reporting in `pjproject/pjsip/src/pjsua-lib/pjsua_call.c`.

**Interfaces:** Add a copied `PjsuaCallTransactionRecord` containing native ID, method, role, CSeq, terminal transaction status/SIP code, and transport result. Add `PjsuaCallManager::OnCallTransaction(const PjsuaCallTransactionRecord &) noexcept`. Correlate only the current local hold/resume INVITE. Retain `call_held`/`call_resumed` notifications with an error for failed operations; do not manufacture a call timeout transition.

- [ ] Add `rejected_hold_completes_once_preserving_established` and `rejected_resume_completes_once_preserving_hold`. A scripted 488 must produce one failed operation carrying SIP 488, preserve the existing call and its prior hold reason, and permit a later successful control. Capture RED on current code.
- [ ] Route `on_call_tsx_state` through synchronous value copies. Distinguish initial INVITE, remote re-INVITE, provisional states, authentication/retry transactions, and the pending local control. Install correlation before native dispatch to handle synchronous callbacks. Duplicate/late callbacks must not complete a newer operation or a reused native ID.
- [ ] Report final rejection/timeout/transport failure through the pending media-result record. Keep success pending until negotiated media confirms it. Map 488/unusable SDP to `negotiation_failed`, ordinary remote rejection to `remote_rejected`, transport failure to `signaling_failed`, and transaction timeout to `timed_out`, without moving an established call through the UML's initiation timeout edge.
- [ ] Cover a 2xx response whose SDP negotiation fails and emits no media-state callback. Where transaction data alone cannot expose the failure, add a narrow optional native `on_call_media_update_result(pjsua_call_id, pj_status_t)` callback at the final negotiation-result boundary. Copy it immediately, correlate it to the active control, and coalesce with transaction failure so exactly one result is emitted. Do not infer successful negotiation solely from SIP 200.
- [ ] Test 488, 500, transport close, no response, duplicate result, stale CSeq/native ID, failed SDP in 200, and hangup racing completion. Preserve the current hangup-cancels-pending-control escape. Confirm operation/event reservations return and unaffected calls remain usable.
- [ ] Run the host, call-component, and real multi-call profiles; commit as `fix(voip): resolve rejected media control operations`.

**Corrective milestone gate:** Tasks 1–2 pass before calling the implementation compliant with the Plan 4 resource and operation contracts. Do not erase the older acceptance evidence; append the discovered gaps and their new regression results.

## Task 3: Implement the bounded PCM port using the existing contract

**Files:** Create `voip/src/pjsua/PjsuaAudioPort.hpp/.cpp`, `voip/tests/pjsua/PjsuaAudioPortTest.cpp`, `PjsuaPlan5TestMain.cpp`, `applications/voip_integration/pjsua_media_component.conf`; extend `voip/include/voip/PcmAudio.hpp`, `voip/tests/unit/AgentRegistryTest.cpp`, `applications/voip_integration/Kconfig`, `CMakeLists.txt`, and `voip/zephyr/CMakeLists.txt`. Modify `AgentRegistry.cpp` only for a demonstrated contract gap.

**Interfaces:** Preserve `PcmSource::Read(int16_t *, size_t, uint64_t) noexcept`, `PcmSink::Write(const int16_t *, size_t, uint64_t) noexcept`, `Format()`, and `Flush()`. Add `Error PjsuaAudioPort::Initialize(AgentAudioBinding, PcmFormat) noexcept`, `pjmedia_port *Port() noexcept`, `void BeginStop() noexcept`, `bool IsQuiescent() const noexcept`. Timestamp units are conference sample ticks, not milliseconds or encoded RTP codec ticks.

- [ ] Extend existing format tests for mismatched rates/frame sizes/channels/sample format, null bindings, 8 kHz/80 and 160 samples, and arithmetic boundaries. Reuse already passing validation; do not claim existing APIs are absent or add `ServiceConfig::frame_time_ms`.
- [ ] Add callback tests for exact-size signed-16 frames, timestamp forwarding, non-audio frames, invalid/null buffers, stop races, partial source writes followed by failure, source underflow, and sink overflow. Run the new profile to establish RED for the absent port.
- [ ] Implement a stable custom `pjmedia_port` with copied format, borrowed binding, and bounded atomic state/counters. Increment callback-entry count before observing stopping state; decrement on every exit. A failed capture zeroes the entire expected output frame; busy/resource exhaustion yields silence/drop without escalating a normal transient underrun/overrun.
- [ ] Latch the first non-transient endpoint error in one stable atomic mailbox slot per bridge (two total). Define it as a fatal endpoint failure; do not use an unspecified “persistent” threshold. Actor consumption validates call/bridge generation. Callback code performs no allocation, logging, blocking wait, PJSUA control, or public event publication.
- [ ] Register production sources exactly once and create an isolated Plan 5 component target. Reuse static test storage. Verify direct callbacks and concurrent stop tests; run a host sanitizer variant where the PJMEDIA fixture supports it and report unavailable sanitizer coverage explicitly.
- [ ] Commit as `feat(voip): add bounded custom PCM ports`.

## Task 4: Drive the no-device conference explicitly

**Files:** Create `voip/src/pjsua/PjsuaMediaClock.hpp/.cpp`, `voip/tests/pjsua/PjsuaMediaClockTest.cpp`; modify `PjsuaRuntime.hpp/.cpp`, `PjsuaRuntimeAdapter.cpp`, `PjsuaApi.hpp/.cpp`, component fakes and both source manifests.

**Interfaces:** Retain the master port returned by `set_no_sound`. Add `Error PjsuaMediaClock::Initialize(pjmedia_port *, PcmFormat) noexcept`, `Error Pump(uint64_t now_ms) noexcept`, `void Stop() noexcept`. The adapter calls `Pump` on the actor. This increment uses actor-driven clocking, with timing quality measured separately from hardware acceptance.

- [ ] Write deterministic tests: at 8 kHz/160 samples, zero frames before the first 20 ms deadline, exactly one at 20 ms, 160-tick timestamp increments; repeat at 80 samples/10 ms. A late actor turn processes at most one frame and skips missed deadlines without an unbounded catch-up loop; timestamp progression reflects skipped sample periods.
- [ ] Retain and drive the conference master interface with silent master input and a discarded output frame. No application source/sink connects to conference slot zero. Driving the master port for timing is distinct from routing a call through slot zero.
- [ ] Allocate the reusable frame buffer from the fixed PJ arena before initialization succeeds, sized from the validated format, with checked byte arithmetic. Reuse it each tick; no fixed 160-sample assumption for other already accepted frame sizes. Add fakeable `pjmedia_port_get_frame`/`put_frame` wrappers with exact checked-in signatures to the native seam.
- [ ] Call the clock from the actor-owned pump with nonblocking SIP event polling. Stop ticks before destroying the conference or arena. On master-frame failure, send one bounded internal failure notification and retain live ownership during cleanup.
- [ ] Test duplicate pump time, clock counter wrap, large scheduling gap, initialization rollback, shutdown/reinitialize, and zero callbacks after stop. Measure actor lateness under two calls and registration refresh; record it without claiming production audio timing.
- [ ] Run the Plan 5 component plus Plan 3 runtime regressions; commit as `feat(voip): drive no-device conference media clock`.

## Task 5: Implement direct bridge connections and safe detach

**Files:** Create `voip/src/pjsua/PjsuaMediaBridge.hpp/.cpp`, `voip/tests/pjsua/PjsuaMediaBridgeTest.cpp`; modify `PjsuaApi.hpp/.cpp`, `FakePjsuaApi.hpp`, and production/component source manifests.

**Interfaces:** Add native seam entries matching the checked-in `pjsua_conf_add_port`, `pjsua_call_get_conf_port`, `pjsua_conf_connect`, `pjsua_conf_disconnect`, and `pjsua_conf_remove_port` signatures. Add bridge `Error Attach(CallHandle, AgentHandle, uint32_t token, pjsua_call_id, AgentAudioBinding, PcmFormat) noexcept`, `void BeginStop() noexcept`, `Error PollStop() noexcept`, and `bool IsFree() const noexcept`. `PollStop` returns `busy` while ownership remains reachable, `ok` after release, or a concrete native cleanup error while retaining storage.

- [ ] Test attach ordering and each failure boundary: port initialization, conference registration, slot resolution, transmit connect, receive connect. No edge may connect one call/bridge to another or route application audio through port zero.
- [ ] Implement free/binding/active/stopping/quiescing ownership with recorded native IDs, logical generation, connection flags, and stable `PjsuaAudioPort`. Invalid or not-yet-ready native slots defer attachment rather than connecting an invalid ID.
- [ ] Use one stateful cleanup path for normal detach and partial rollback. Mark stopping, disconnect known edges, request native stream teardown via the call manager, wait for callback and conference-operation quiescence, remove the custom port, flush once, and only then mark free. Failed disconnect/remove retains ownership for retry; it never frees a callback-reachable record.
- [ ] Verify the checked-in conference implementation's documented remove/connect completion semantics. If operations complete asynchronously, use its operation-completion/group-lock lifecycle signal; a successful enqueue return or a momentary callback count of zero is insufficient. Keep backing storage stable through that completion.
- [ ] Test callback entry racing stop, outstanding remove, disconnect failure, repeated stop, partial rollback, native call-slot replacement, and reuse with a different agent/generation. The sink must not flush concurrently with its last write.
- [ ] Run all Plan 5 components; commit as `feat(voip): add quiescent direct media bridges`.

## Task 6: Integrate two bridges with call ownership and shutdown

**Files:** Create `voip/src/pjsua/PjsuaMediaManager.hpp/.cpp`, `MediaSecurityPolicy.hpp`, `voip/tests/pjsua/PjsuaMediaManagerTest.cpp`; modify `PjsuaRuntimeAdapter.hpp/.cpp`, `PjsuaCallManager.hpp/.cpp`, `PjsuaCallContext.hpp`, `PjsuaCallbackRouter.hpp/.cpp`, `voip/src/core/RuntimeAdapter.hpp`, `FakeRuntimeAdapter.hpp/.cpp`, `VoipRuntime.cpp`, and associated component/build files.

**Interfaces:** `PjsuaMediaManager` owns exactly two bridges. Provide `Error OnPromoted(CallHandle, AgentHandle, uint32_t token) noexcept`, `void OnMediaState(uint32_t token, const CopiedMediaState &) noexcept`, `void BeginStop(uint32_t token) noexcept`, `bool IsQuiescent(uint32_t token) const noexcept`, `void Pump() noexcept`, and `uint16_t Available() const noexcept`. `CopiedMediaState` contains native ID, local direction, native media status, and conference slot; no callback-local pointers. Add actor-only `RuntimeAdapter::AvailableMediaBridges() const noexcept` to all adapters for truthful resource snapshots.

- [ ] Test agents 0 and 4 with unique source/sink sentinels, no bridge on queued calls, third acquisition refusal, stale agent generation, two simultaneous error reports, and reuse only after complete teardown.
- [ ] Preconstruct both bridges in the adapter. Resolve bindings through `AgentRegistry::Resolve(AgentHandle)` after promotion; cache no account-owned audio pointer or registry context across lifecycles. Store an explicit native promotion fact, separate from public business state, so waiting-but-promoted calls are eligible and queued calls are not.
- [ ] Implement `MediaSecurityPolicy::Validate(MediaSecurity) noexcept`: `none` returns `ok`, `srtp_sdes` returns `unsupported_configuration`, and unknown enum values return `invalid_argument`. Pass the initialized policy into media creation; test that rejection has no native side effects and does not enable SRTP sources.
- [ ] Copy media observations in callbacks and process attach/detach on the actor. Handle media-before-confirmation, confirmation-before-media, and duplicate notifications idempotently. Keep bridge error reports independently latched until consumed.
- [ ] Split native-stream inactivity from custom-bridge quiescence in the call release gate. `TryRelease` may publish finish only when both are satisfied. On bridge/media failure, tear down only the affected call and retain its lease until cleanup completes.
- [ ] Stop admission, quiesce bridges, finish native calls, unregister accounts, stop the conference clock, destroy runtime pools, erase credentials, and publish final `service_stopped` in that ownership order. A timed-out shutdown keeps all reachable objects live and supports a later cleanup retry.
- [ ] Replace the bridge-count proxy in `VoipRuntime::RefreshResources` with actual adapter availability. Test a promoted signaling call without an attached bridge, both attached, one quiescing, and fully free after shutdown. Keep resource guards and public snapshots consistent without invoking PJSUA from snapshot readers.
- [ ] Check the existing `sizeof(PjsuaRuntimeAdapter) < 20480` and 131072-byte service storage bounds. Measure production and component stacks; use static fixtures or shard tests before considering storage increases. Run host, Plan 3 fake, Plan 4 component/multi-call, and Plan 5 component tests.
- [ ] Commit as `feat(voip): integrate two owned agent media bridges`.

## Task 7: Apply negotiated direction and preserve failed-control state

**Files:** Modify `PjsuaMediaBridge.hpp/.cpp`, `PjsuaMediaManager.cpp`, `PjsuaCallManager.cpp`; create `voip/tests/pjsua/PjsuaMediaDirectionTest.cpp`; extend Plan 5 test registration.

**Interfaces:** `Error PjsuaMediaBridge::ApplyDirection(pjmedia_dir local_direction) noexcept`. Encoding means custom-to-call; decoding means call-to-custom. Media operation success is published after negotiation and successful bridge reconciliation, not just native API return.

- [ ] Test `ENCODING_DECODING` as both directions, `ENCODING` as transmit only, `DECODING` as receive only, and `NONE` as neither. Separately feed remote SDP sendonly/recvonly and confirm that PJSUA's negotiated local direction produces the opposite remote-facing role exactly once.
- [ ] Test local hold/resume success, all Task 2 failures, remote direction changes without a local operation, duplicate observations, and media slot replacement. Failed control preserves the previous business state, hold reason, and usable connections; successful resume flushes before reception reconnects.
- [ ] Apply only changed conference edges. Re-resolve replaced slots and remove old edges before connecting the new slot. A connection failure starts affected-call teardown instead of reporting a successful operation with unusable audio.
- [ ] Confirm `hold(waiting)` cannot trigger media-hold control and a duplicate media callback cannot complete an unrelated operation. Remote direction changes must update bridge routing and copied media flags without inventing a local hold command.
- [ ] Run Plan 5 components and the real hold/re-INVITE matrix; commit as `feat(voip): reconcile negotiated media directions`.

## Task 8: Prove two isolated RTP audio paths

**Files:** Create `applications/voip_integration/src/multi_agent_audio.cpp`, `multi_agent_audio.conf`, `test_support/DeterministicPcm.hpp`, `test_support/ScriptedRtpPeer.hpp/.cpp`; extend `ScriptedSipPeer.hpp/.cpp`, application Kconfig/CMake, and existing heap tracking integration.

**Interfaces:** Two bounded RTP peers negotiate distinct ports/SSRCs and G.711 streams. Five preallocated PCM bindings provide distinct signals and independent sink evidence. Use the public service for control; use actor-copied test instrumentation only for otherwise invisible resource assertions.

- [ ] Establish simultaneous calls for agents 0 and 4 with independent transmit and receive patterns. Require at least 250 frames in each direction per active path, RTP sequence/timestamp progression, and RTCP exchange. Since G.711 is lossy, compare with encode/decode reference output or use companding-stable patterns, not raw pre-encoding PCM hashes.
- [ ] Saturate the five-entry mixed FIFO with both leases occupied. Include offered and offerless incoming calls. Assert zero additional media transports/codecs/custom ports/bridges for queued entries, correct 180/486 behavior, head blocking, and ordered promotion after complete teardown.
- [ ] Exercise remote sendonly/recvonly/inactive, successful/rejected hold/resume, source underflow, sink overflow, fatal failure of one endpoint with the other call continuing, peer BYE/CANCEL, delayed callbacks, and bridge reuse by another agent. Assert zero cross-agent samples in both directions.
- [ ] Run five complete initialization/call/media/shutdown lifecycles with general-heap wrappers. Require zero post-init allocations, zero callbacks after stop, two free bridges, removed custom ports, correct flush ordering, arena return to the runtime baseline after calls and zero usage/live blocks after runtime destruction. Count native RTP transports, not just logical calls.
- [ ] Build and run using the common command with `multi_agent_audio.conf`, `/tmp/voip-multi-audio`, and a 120-second wrapper. Require `VOIP MULTI AUDIO RESULT: PASSED (2 isolated bridges)` and a clean full log. Record clock lateness and stack/arena peaks alongside the verdict.
- [ ] Commit as `test(voip): qualify two isolated RTP audio paths`.

## Task 9: Run the acceptance matrix and reconcile roadmap status

**Files:** Create `docs/voip/pjsua-custom-audio-media-bridge.md`; update `docs/voip/pjsua-call-management-promotion.md`, `docs/voip/pjsua-zephyr-port.md`, the old Plan 5 document, and the Plan 6 document. Add a current-track pointer to `docs/VOIP_PJPROJECT_INTEGRATION_PLAN.md` and mark `docs/voip/plan4-session-handoff.md` historical; preserve their original evidence and scope.

- [ ] Run two full host suites at the final source revision. Rebuild and run `pjsua_link.conf`, `pjsua_arena.conf`, `pjsua_capacity.conf`, `pjsua_registration_fake.conf`, `pjsua_registration.conf`, `pjsua_call_component.conf`, `multi_call_scheduler.conf`, `pjsua_media_component.conf`, and `multi_agent_audio.conf`. Use exact markers from their current acceptance records/test mains and whole-log fault checks. Do not present timeout alone as success.
- [ ] Check new failure cases as well as the legacy verdicts: no queued RTP allocation, failed re-INVITE completion, real clock-driven media, direction/isolation, late-callback teardown, and truthful bridge counts.
- [ ] Build both `pjsua_media_component.conf` and the real production-linked `multi_agent_audio.conf` for `mimxrt1064_evk`; obtain `ram_report` and `rom_report`. If the real profile needs network configuration, record the specific build blocker and leave that gate incomplete rather than substituting a fake-only result. Do not inspect external Zephyr sources.
- [ ] Record the exact tested Git revision, commands, build and run exit codes, complete-log hashes, linker RAM/ROM, arena current/peak/largest-free/live/failure statistics, actor/test stack headroom, clock cadence, and heap/callback cleanup. Normal arena peak must remain at or below 75%; injected failures below 90%. Increasing test RAM or pool limits is not a passing result.
- [ ] Update Plan 5 to point to this executed sequence and the new acceptance record. Document proposed native extensions as implemented only after their tests pass. Explain the master-clock use of port zero while retaining the prohibition on cross-call audio routing through it.
- [ ] Rebase Plan 6 file lists and RED expectations onto completed code: extend the existing resource guard, shutdown paths, diagnostics configuration, and tests. Preserve its 100-lifecycle soak, pressure/fault matrix, real-PBX interoperability, target RAM sizing, hardware audio/CPU/latency/DMA work, and migration gates. Remove compatibility code only after replacement parity; legacy retirement remains separate.
- [ ] Review the whole change against the approved spec and these findings, repair confirmed load-bearing defects, rerun affected gates, and commit the acceptance record as `docs(voip): accept corrected PJSUA media integration`.

## Completion and following plan

The immediate corrective milestone is complete only when Tasks 1–2 have reproducible RED/GREEN evidence and their host/component/real-SIP regressions pass. Plan 5 is complete only when Tasks 3–9 prove actual timed audio, two isolated bridges, no queued media allocation, correct operation outcomes, and quiescent cleanup on the final revision.

After those gates, continue with `docs/superpowers/plans/2026-08-27-voip-robustness-migration-qualification.md`, rebased as Task 9 specifies. Do not restart the historical facade Phase 0 or infer production acceptance from the current QEMU signaling tests.
