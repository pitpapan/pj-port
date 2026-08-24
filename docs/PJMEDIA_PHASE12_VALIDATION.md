# PJMEDIA Phase 12 Robustness and Resource Validation

Date: 2026-08-24

## Current result

The initial Phase 12 robustness profile passes on `mps2/an385` under QEMU:

```text
PHASE 12 ROBUSTNESS PROFILE: PASSED (extended lifecycle)
```

The supported embedded profile is intentionally bounded to one concurrent SIP
call and two G.711 streams. Phase 12 uses one extended lifecycle containing two
120-frame bidirectional active calls plus the complete signaling failure and
recovery matrix. Repeated complete initialization/shutdown remains covered by
the three-lifecycle Phase 11 regression gate.

No Zephyr source file was inspected or modified.

## Coverage

- pre-SDP RTP allocation and negotiated peer-port binding;
- 120 generated 20 ms frames per active-call direction;
- bidirectional PCMU RTP, telephone event, pause/resume, RTCP statistics, and
  jitter-buffer state;
- local and remote BYE, CANCEL/487, 4xx/6xx rejection, dropped INVITE and
  retransmission, unused-port timeout, offerless INVITE, re-INVITE hold and
  incompatible SDP, registration/OPTIONS concurrency, and abrupt cleanup;
- malformed SIP and SDP input inherited from the Phase 6 matrix;
- transaction, timer, transport, callback, PJ pool, heap, and stack accounting;
- explicit media-transport stop before socket/ioqueue destruction.

Representative results:

```text
[Phase 12] cadence max lateness=10 ms across 120 frames: PASSED
[Phase 6] resources: PJ heap peak=272480 B/65 blocks; max transactions=4, timers=6, transports=2, UDP sockets peak=3, PJSIP ioqueue handles=2
[Phase 6] lifecycle 1 complete teardown: PASSED
```

Final image:

```text
FLASH: 309876 B / 4 MB
RAM:   1155952 B / 4 MB
```

Build and run:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase12 -- -DEXTRA_CONF_FILE=phase12_robustness.conf
timeout --signal=TERM --kill-after=3s 30s \
  west build -d build-pjmedia-phase12 -t run
```

## Scope boundary

This establishes the initial supported QEMU headless-media limit. The full
Phase 12 exit gate remains open for controlled allocator failure, malformed
RTP/RTCP injection while a stream is active, sustained loss/reorder/burst
injection, and CPU-utilization measurement. It does not claim multiple
concurrent calls, product-board network behavior, physical audio, ICE, SRTP,
IPv6, or video.
