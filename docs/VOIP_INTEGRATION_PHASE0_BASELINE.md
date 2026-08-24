# VoIP Integration Phase 0 Baseline

Date: 2026-08-24

## Result

Phase 0 freezes the initial PJPROJECT VoIP integration requirements and the
reusable port-validation baseline.  It does not add the C++ facade or change
PJPROJECT protocol sources.

The integration target is one account and one call, with SIP over IPv4 TCP,
RTP/RTCP over IPv4 UDP, PCMU/PT 0 and PCMA/PT 8, and 8 kHz mono signed 16-bit
PCM in 20 ms frames.  QEMU uses generated PCM and a bounded memory sink.  The
product target is MIMXRT1060 with ADC/eDMA capture and SPI DAC playback.

## Workspace baseline

| Item | Recorded value |
| --- | --- |
| Workspace | `/home/pitpapan/zephyrproject` |
| Workspace revision | `f2924d60458d4b1acc1c3dceb3ac14140097da51` |
| Worktree at entry | Untracked west-managed `bootloader/`, `modules/`, `tools/`, and `zephyr/`; untracked integration plan |
| PJPROJECT | 2.16 |
| Zephyr | 4.4.0, as established by the completed validation records |
| west | 1.5.0 |
| Python | 3.12.13 from `.venv` |
| CMake | 4.4.2 |
| Ninja | 1.10.1 |
| Zephyr SDK | 1.0.1 |
| Compiler | Zephyr SDK GCC 14.3.0 |
| Development target | `mps2/an385` under QEMU |

The Zephyr tree is an external dependency.  No file below `zephyr/` was read,
searched, indexed, or modified during this phase.

## Requirements matrix

| Requirement | Initial disposition | Validation gate |
| --- | --- | --- |
| Replace custom SIP signaling | PJSIP owns parsing, transport, transactions, authentication, registration, dialogs, and INVITE usage | Integration Phases 2-5 and 7 |
| Replace custom RTPAudio | PJMEDIA owns SDP, G.711, RTP/RTCP, jitter buffering, streams, and timing | Integration Phases 6-9 |
| Existing-compatible public API | C++ facade and compatibility adapters; no PJ type in public headers | Phase 1 compile and compatibility matrix |
| One account | One fixed account slot; second configuration is rejected or replaces only while inactive | Phases 3, 4, and 9 |
| One call | One fixed call slot; a second call attempt fails cleanly | Phases 5, 7, and 9 |
| SIP over TCP mandatory | Production facade explicitly selects IPv4 TCP; UDP remains validation-only | Phases 2, 4, 5, 7, and 9 |
| RTP/RTCP over UDP | One RTP/RTCP transport pair per call | Phases 6, 7, and 9 |
| PCMU and PCMA first | Static PT 0 and PT 8 only, 8 kHz mono | Phases 5-9 |
| PCM contract | Signed 16-bit mono, 8 kHz, 20 ms/160-sample frames | Phases 6-11 |
| QEMU audio | Deterministic generated source and bounded count/hash sink | Phases 6, 7, and 9 |
| Product capture | ADC/eDMA adapter; no hardware type in signaling API | Phases 10-11 |
| Product playback | SPI DAC adapter; no hardware type in signaling API | Phases 10-11 |
| Shared runtime | One PJLIB/PJSIP/PJMEDIA lifecycle, ioqueue, event thread, and TCP factory | Phase 2 onward |
| Bounded commands and callbacks | Fixed-capacity queue; callbacks serialized outside PJ locks and invalid after shutdown | Phases 1-3 and 9 |
| Credential safety | Bounded copied configuration; credentials and Authorization values excluded from normal logs | Phases 1, 3, 4, and 9 |
| Registration | Digest challenge, refresh, unregister, TCP loss, bounded reconnect | Phases 3, 4, and 9 |
| Call control | Incoming/outgoing, delayed accept, reject, CANCEL, BYE, re-INVITE hold/resume | Phases 5, 7, and 8 |
| Upstream integrity | Use public PJPROJECT APIs; patch protocol sources only for a reproduced compatibility defect with a test | Every phase |
| Deferred features | TLS, SRTP, ICE/TURN/STUN, IPv6, video, presence, IM, conference, extra codecs | Outside initial release gate |

## Existing API compatibility inputs

The facade must account for these documented legacy operations and events:

- `SIPSessionManager::CreateRegistrable()`;
- `CreateOutgoingDirectCall()` and misspelled legacy
  `CreateIncommingDirectCall()`;
- `OrderCall()`/`OrderMakeCall()`, `OrderAcceptCall()`, and `OrderAbortCall()`;
- `PendingIncomingCall()`, `EstablishCall()`, `CallEnd()`, `SessionFail()`, and
  `ReInvite()` observer events;
- access to copied local/remote negotiated media address, RTP/RTCP ports,
  codec, and direction in place of exposing mutable PJ SDP objects;
- `GetSessionsInfo()` and `GetSessionsSequences()` if applications still use
  them; and
- media start, stop, and hold behavior formerly reached through
  `RTPAudioManager`.

The legacy source files referenced by `docs/voip_stack.md` are not present in
this workspace.  The Phase 1 compatibility matrix therefore uses that document
as its current evidence and must be checked against the product repository
before source-compatibility is claimed.

## Reusable completed validations

| Evidence | Reusable result | Integration limitation |
| --- | --- | --- |
| PJLIB Stages 8-10 | Core runtime, IPv4 UDP/TCP, and select ioqueue with 32 handles | Not an integrated facade lifecycle |
| PJSIP Phase 11 | Registration/Digest, IPv4 UDP signaling, concurrency, malformed input, exhaustion, and five lifecycles | Mandatory facade TCP registration/calls remain unproven |
| PJMEDIA Phases 1-3 | Build boundary, SDP parse, and offer/answer | No facade ownership |
| PJMEDIA Phases 4-6 | INVITE lifecycle, loop calls, and IPv4 UDP call control | SIP TCP call control remains unproven |
| PJMEDIA Phases 7-9 | PCMU/PCMA, RTP/RTCP/jitter, loop and UDP media transports | No stream until Phase 11 |
| PJMEDIA Phase 11 | Integrated SIP-controlled bidirectional generated-PCM PCMU media | Uses SIP UDP and a test harness, not the public facade |
| PJMEDIA Phase 12 | Extended headless call robustness profile | Full Phase 12 gate remains open as documented |

## Open product decisions

These are deliberately not selected in Phase 0:

- exact legacy header namespace, signatures, and return types from the product
  repository;
- whether direct incoming/outgoing session factories remain supported in the
  first product release or only compile as compatibility adapters;
- maximum URI, username, password, reason-text, and copied-SDP field lengths;
- application callback execution context if it must differ from the facade
  event thread, and whether observers may issue commands reentrantly;
- redirect, proxy authentication, TCP keepalive, registrar failover, and
  reconnect/backoff product policy;
- whether RFC 2833 DTMF is a release requirement;
- flash, static RAM, heap, pool, stack, latency, and soak acceptance budgets;
- credential persistence and secure-storage ownership;
- RTP port range, entropy, SSRC, sequence, and timestamp policy;
- selected registrar/proxy and interoperability peer; and
- physical MIMXRT1060 board, ADC channel/timer/rate contract, SPI DAC model,
  DMA/cache ownership, and acceptable clock drift.

## Phase 0 exit assessment

All fixed architectural decisions are mapped to later tests or explicit
hardware/product gates.  The port validations establish reusable components,
but do not prove the mandatory SIP/TCP facade path.  Phase 1 may begin after
the clean regression results appended below pass; product-source compatibility
claims remain gated on obtaining the legacy public headers.

## Clean regression evidence

Both profiles were configured and built from scratch on 2026-08-24.  QEMU
entered idle after each pass marker, so exit status 124 from the bounded runner
is expected.

### PJSIP signaling/resource profile

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-voip-phase0-pjsip -- \
  -DEXTRA_CONF_FILE=phase11_robustness.conf
timeout --signal=TERM --kill-after=5s 120s \
west build -d build-voip-phase0-pjsip -t run
```

Build result: 255,280 B flash and 602,904 B RAM.  Runtime result:

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

The run measured a 290,384 B / 80-block PJ heap peak, 16 transactions, 24
timers, two transports, and zero live PJ pools after teardown.

### PJMEDIA integrated headless profile

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-voip-phase0-pjmedia -- \
  -DEXTRA_CONF_FILE=phase12_robustness.conf
timeout --signal=TERM --kill-after=3s 30s \
west build -d build-voip-phase0-pjmedia -t run
```

Build result: 309,944 B flash and 1,155,952 B RAM.  Runtime result:

```text
PHASE 6 RESULT: PASSED (1 complete IPv4 UDP call lifecycles)
PHASE 12 ROBUSTNESS PROFILE: PASSED (extended lifecycle)
```

Both generated-PCM directions delivered 119 frames in each extended active
call, with the expected content hashes, RTP/RTCP statistics, telephone event,
pause/resume, and a maximum observed cadence lateness of 10 ms.  Teardown
returned transactions, timers, transports, callbacks, and PJ pools to the
validated baseline.

### Phase conclusion

Phase 0 passes.  Phase 1 is the next permitted increment.  The strongest
existing media profile remains SIP-over-UDP, so it cannot substitute for the
mandatory SIP-over-TCP integration tests.
