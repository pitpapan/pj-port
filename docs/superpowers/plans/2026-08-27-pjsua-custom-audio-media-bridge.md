# PJSUA Custom Audio and Media Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect each of the two promoted PJSUA calls directly to the immutable microphone/speaker binding of its agent using preallocated custom PJMEDIA conference ports.

**Architecture:** PJSUA runs with no platform sound device. `PjsuaMediaManager` owns two stable `PjsuaMediaBridge` records. Each bridge owns one custom bidirectional `pjmedia_port`; its `get_frame` pulls nonblocking signed-16 PCM from the agent source and its `put_frame` pushes received PCM to the agent sink. The custom conference port connects only to its call slot in both directions, so calls never mix with one another.

**Tech Stack:** C++17, PJMEDIA port/conference APIs, PJSUA conference API, fixed buffers, atomics for callback quiescence, fake media API host tests, G.711 RTP/RTCP UDP validation.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Business state machine:** `docs/superpowers/specs/state_machine.uml`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- Keep `pjsua_set_no_snd_dev()`; do not bind a global sound device.
- Preallocate exactly two bridges; queued calls allocate no media bridge or RTP resource.
- Resolve audio through `AgentHandle -> AgentRegistry -> AgentAudioBinding` only at promotion.
- Never store audio objects in `PjsuaAccountContext`.
- Support only the initialized signed-16 mono conference format in this increment.
- Do not design codec priority/selection; use PJSUA's negotiated audio stream. Integration validation may use G.711.
- Use plain RTP/RTCP UDP; keep SRTP disabled but preserve the policy boundary.
- This plan may produce only `hold(media)` after negotiated hold. It must never
  interpret or mutate pre-establishment `hold(waiting)`, which belongs to call
  scheduling/control even after promotion and before acceptance.

---

## Task 1: Finalize and validate the PCM endpoint contract

**Files:**

- Modify: `voip/include/voip/PcmAudio.hpp`
- Modify: `voip/src/core/AgentRegistry.cpp`
- Create: `voip/tests/unit/PcmAudioContractTest.cpp`

**Interfaces:**

- Consumes: `PcmFormat`, application-owned source/sink implementations.
- Produces: format validation and nonblocking source/sink/flush semantics.

- [ ] Write test doubles exposing format and fixed rings. Test matching formats, mismatched sample rate/frame size/channels/sample format, null endpoints, source underflow, sink overflow, and sink flush.

- [ ] Confirm incompatible formats currently pass initialization or fail to compile.

- [ ] Define `SampleFormat::signed_16`, `PcmSource::Format()`, `PcmSource::Read(samples,count,timestamp)`, `PcmSink::Format()`, `PcmSink::Write(...)`, and `PcmSink::Flush()`. All methods are `noexcept` and nonblocking.

- [ ] During `AgentRegistry` validation, require source and sink formats to equal `ServiceConfig::conference_format`, require mono signed-16, and validate `samples_per_frame == sample_rate_hz * frame_time_ms / 1000` without overflow. Initial product config is 8000 Hz, 160 samples, mono, signed-16.

- [ ] Specify runtime error semantics: source `busy/resource_exhausted` produces silence for that frame; sink `busy/resource_exhausted` drops that frame; persistent failures are reported through bounded media-control notifications, never thrown from a PJMEDIA callback.

- [ ] Run `PcmAudioContractTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/include/voip/PcmAudio.hpp voip/src/core/AgentRegistry.cpp voip/tests/unit/PcmAudioContractTest.cpp
  git commit -m "feat(voip): finalize bounded PCM endpoint contract"
  ```

## Task 2: Implement a custom bidirectional `pjmedia_port`

**Files:**

- Create: `voip/src/pjsua/PjsuaAudioPort.hpp`
- Create: `voip/src/pjsua/PjsuaAudioPort.cpp`
- Create: `voip/tests/pjsua/PjsuaAudioPortTest.cpp`

**Interfaces:**

- Consumes: one borrowed `AgentAudioBinding`, `PcmFormat`, PJMEDIA frame callbacks.
- Produces: stable `pjmedia_port`, atomic callback counts, audio error counters.

- [ ] Write direct callback tests for a normal capture frame, source underflow/silence, normal playout, sink overflow/drop, non-audio frame handling, wrong frame size, monotonically converted timestamps, stopping behavior, and concurrent callback-count quiescence.

- [ ] Confirm the tests fail because `PjsuaAudioPort` is absent.

- [ ] Define `PjsuaAudioPort` with the `pjmedia_port` as its first/stably addressed member, fixed source/sink pointers, copied format, atomics `running`, `stopping`, and `callbacks_in_flight`, and counters for captured, played, silent, dropped, and failed frames.

- [ ] Initialize `pjmedia_port_info` with `PJMEDIA_SIG_CLASS_PORT_AUD`, signed-16 PCM format, exact clock/channel/samples/bits values, and static `get_frame`, `put_frame`, and destroy callbacks.

- [ ] In each static callback, increment the in-flight counter before checking state and decrement on every exit. `get_frame` fills the exact frame buffer; on source failure it zeroes all samples and returns `PJ_SUCCESS`. `put_frame` never blocks and returns `PJ_SUCCESS` after counting a dropped application frame.

- [ ] Send persistent endpoint failure to a fixed two-record media-control mailbox using an atomic one-shot latch; do not publish a public event or call the actor directly from the media callback.

- [ ] Implement `BeginStop()` and `IsQuiescent()`. Do not destroy or rebind the port until `callbacks_in_flight == 0`.

- [ ] Run `PjsuaAudioPortTest` under normal and thread-sanitized host configurations where available; require deterministic counter totals.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaAudioPort.* voip/tests/pjsua/PjsuaAudioPortTest.cpp
  git commit -m "feat(voip): add custom PJMEDIA PCM port"
  ```

## Task 3: Register and connect one direct call bridge

**Files:**

- Create: `voip/src/pjsua/PjsuaMediaBridge.hpp`
- Create: `voip/src/pjsua/PjsuaMediaBridge.cpp`
- Create: `voip/tests/pjsua/PjsuaMediaBridgeTest.cpp`

**Interfaces:**

- Consumes: custom port, PJSUA call conference slot, call/agent handles.
- Produces: one direct bidirectional conference connection and ordered teardown.

- [ ] Write a fake conference API test checking exact attach order: initialize port, `pjsua_conf_add_port`, resolve call slot, connect custom source port to call slot, connect call slot to custom sink port. Verify rollback after failure at each step.

- [ ] Add an isolation assertion that no connection uses conference port 0 and no bridge connects to another bridge or another call slot.

- [ ] Confirm tests fail because the bridge is absent.

- [ ] Define stable bridge state: free, binding, active, stopping, quiescing. Store call handle, agent handle, native call ID, custom conference port ID, call conference port ID, connection flags, and `PjsuaAudioPort`.

- [ ] Implement attachment only after `pjsua_call_get_conf_port(call_id)` returns a valid active audio slot. Register with `pjsua_conf_add_port()`, then connect `custom_port -> call_port` and `call_port -> custom_port`.

- [ ] On any partial failure, disconnect completed directions in reverse order, remove the custom port, wait for callback quiescence if it became visible, flush sink, and return `media_failed` without releasing the promoted call itself.

- [ ] Implement detach order exactly: mark stopping, disconnect both directions, request stream stop through call manager, wait for callback quiescence, remove conference port, flush sink, clear native IDs/handles, mark free.

- [ ] Run `PjsuaMediaBridgeTest` and require every failure-injection cleanup trace matches expected reverse order.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaMediaBridge.* voip/tests/pjsua/PjsuaMediaBridgeTest.cpp
  git commit -m "feat(voip): add direct PJSUA call audio bridge"
  ```

## Task 4: Manage exactly two bridges and resolve agent audio correctly

**Files:**

- Create: `voip/src/pjsua/PjsuaMediaManager.hpp`
- Create: `voip/src/pjsua/PjsuaMediaManager.cpp`
- Create: `voip/src/pjsua/MediaSecurityPolicy.hpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Create: `voip/tests/pjsua/PjsuaMediaManagerTest.cpp`

**Interfaces:**

- Consumes: call media-active/stopped notifications, `AgentRegistry`, two bridge records.
- Produces: call-to-agent bridge binding and media teardown-complete notification.

- [ ] Write tests with five distinct source/sink sentinels. Activate calls for agents 0 and 4 and verify each bridge receives only that agent's pointers; reject a third bridge; release one and verify safe reuse with a new generation.

- [ ] Add a regression test for the approved mapping chain: `pjsua_call_id -> PjsuaCallContext -> AgentHandle -> AgentRegistry::Resolve -> AgentContext::audio -> PjsuaMediaBridge`.

- [ ] Confirm tests fail because the manager is absent.

- [ ] Preconstruct two `PjsuaMediaBridge` instances before service initialization succeeds. `Acquire()` chooses the lowest free bridge and requires the call already owns a promoted lease.

- [ ] Resolve the audio binding fresh through the agent handle. Validate the handle generation; never cache an `AgentContext*` across shutdown/reinitialization.

- [ ] Implement `MediaSecurityPolicy` with `plain_rtp` active and `srtp_sdes` returning `unsupported_configuration` when `CONFIG_VOIP_SRTP=n`. Pass the policy to media creation even though only plain is implemented.

- [ ] Notify `PjsuaCallManager` of media attach success/failure and detach quiescence. A media attach failure begins call teardown; it does not leave an established silent call occupying a slot.

- [ ] Run `PjsuaMediaManagerTest` and require correct pointer isolation and two-slot capacity.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua/PjsuaMediaManagerTest.cpp
  git commit -m "feat(voip): bind two media bridges by agent handle"
  ```

## Task 5: Follow negotiated direction and hold/resume state

**Files:**

- Modify: `voip/src/pjsua/PjsuaMediaBridge.hpp`
- Modify: `voip/src/pjsua/PjsuaMediaBridge.cpp`
- Modify: `voip/src/pjsua/PjsuaMediaManager.cpp`
- Modify: `voip/src/pjsua/PjsuaCallManager.cpp`
- Create: `voip/tests/pjsua/PjsuaMediaDirectionTest.cpp`

**Interfaces:**

- Consumes: negotiated PJSUA media direction and re-INVITE completion.
- Produces: correct conference connections for sendrecv/sendonly/recvonly/inactive and confirmed hold state.

- [ ] Write a direction matrix test:

  - `sendrecv`: custom-to-call and call-to-custom connected.
  - remote `sendonly`: only call-to-custom connected.
  - remote `recvonly`: only custom-to-call connected.
  - `inactive`: both disconnected.

- [ ] Add hold tests proving the public hold operation remains pending until successful re-INVITE, failure preserves previous connections/state, and resume flushes the sink before reception reconnects.

- [ ] Assert successful hold projects `established -> hold` with
  `HoldReason::media`, and successful resume projects `hold -> established`.
  A context with `HoldReason::waiting` must be rejected before any media or
  re-INVITE API is invoked.

- [ ] Confirm tests fail because current attach is always bidirectional and operation completion is early.

- [ ] Translate PJSUA/PJMEDIA direction into a desired connection mask. Apply changes idempotently on the actor; tolerate duplicate media-state callbacks.

- [ ] For local hold/resume, keep the operation record until the re-INVITE transaction/call media state confirms the requested direction. On failure publish one operation failure and restore the prior snapshot/connections.

- [ ] Keep private media direction and bridge state independent of the public
  five-state projection. Update the projection only after negotiated success;
  failed hold/resume preserves both the prior business state and hold reason.

- [ ] On resume, call `PcmSink::Flush()` before reconnecting `call_port -> custom_port` so held audio is not played late.

- [ ] Run `PjsuaMediaDirectionTest` and require all direction and failure cases pass.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua/PjsuaMediaDirectionTest.cpp
  git commit -m "feat(voip): follow negotiated media direction"
  ```

## Task 6: Validate two isolated RTP audio paths

**Files:**

- Create: `applications/voip_integration/src/multi_agent_audio.cpp`
- Create: `applications/voip_integration/multi_agent_audio.conf`
- Create: `applications/voip_integration/test_support/DeterministicPcm.hpp`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: two promoted SIP calls, G.711 plain RTP/RTCP UDP, distinct PCM patterns.
- Produces: end-to-end audio routing, isolation, direction, and teardown proof.

- [ ] Define five fixed PCM sources with distinct deterministic sample patterns and five bounded sinks with distinct rolling hashes. Do not allocate their rings after initialization.

- [ ] Establish simultaneous calls for agents A and E against two local scripted RTP peers. Verify each peer receives only its agent's pattern and each application sink hash matches only the corresponding peer pattern.

- [ ] Attempt a second call for A and verify it remains queued without RTP sockets or a bridge. Verify no samples enter A's sink from E and no conference connection uses global port 0.

- [ ] Exercise remote sendonly/recvonly/inactive, local hold/resume, sink overflow, source underflow/silence, peer teardown, and bridge reuse by a later agent.

- [ ] Build and run:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_multi_audio -- \
    -DEXTRA_CONF_FILE=multi_agent_audio.conf
  west build -d build_voip_multi_audio -t run
  ```

  Expected terminator: `VOIP MULTI AUDIO RESULT: PASSED (2 isolated bridges)`.

- [ ] At final shutdown verify callback counters are zero, both bridges are free, all custom conference ports are removed, both sinks were flushed, and arena usage returns to the post-runtime baseline.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): validate isolated per-agent PJSUA audio"
  ```

## Plan 5 Exit Criteria

- Exactly two preallocated custom PJMEDIA ports connect promoted calls to agent audio.
- Audio mapping is resolved from the call's stable agent handle, never from account-owned device state.
- Both directions connect directly between one call and one custom port; there is no cross-call mixing.
- Queued calls allocate no RTP/media/bridge resources.
- Direction, hold/resume, underflow, overflow, failure rollback, and callback quiescence are tested.
- Only negotiated media hold produces `hold(media)`; the bridge never consumes
  or mutates pre-establishment `hold(waiting)`.
- The active path is plain RTP/RTCP; SRTP remains a disabled policy seam.
