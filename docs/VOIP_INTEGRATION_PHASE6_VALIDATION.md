# VoIP Integration Phase 6 Headless Media Validation

Date: 2026-08-24

## Result

Phase 6 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 6 RESULT: PASSED (3 media lifecycles)
```

The existing-compatible C++ facade now owns a headless PJMEDIA runtime for
generated PCM and a bounded memory sink. No PJMEDIA type crosses the public
interface. The initial media profile is 8 kHz, mono, signed 16-bit PCM in
20 ms (160-sample) frames, encoded as PCMU/PT 0 or PCMA/PT 8 and carried over
UDP RTP.

## Production behavior

`CONFIG_VOIP_PJ_HEADLESS_MEDIA` attaches the media backend to the shared
PJPROJECT lifetime established in Phase 2. It registers G.711 on the shared
PJMEDIA endpoint and creates two explicitly bound loopback UDP transports and
two bidirectional streams for deterministic headless validation.

The facade adds media start, pause/resume, stop, and copied statistics. The
statistics expose generated and received frame counts, RTP packet counts,
memory-sink capacity and peak occupancy, sink hash, and jitter-buffer depth
without exposing PJ pools, ports, streams, transports, or codec objects.
Default backend implementations preserve source compatibility for existing
third-party facade backends.

Shutdown ordering stops and joins the PCM worker, destroys streams, closes UDP
transports, releases the media pool, and only then tears down the shared media
endpoint and PJPROJECT runtime.

## Deterministic media matrix

Each of three complete backend lifecycles proves:

- PCMU/PT 0 and PCMA/PT 8 encode, UDP RTP transmission, jitter buffering,
  decode, and memory-sink delivery;
- deterministic nonzero decoded-sample hashes and RTP/frame counters;
- an eight-frame bounded sink whose peak never exceeds its capacity;
- rejection of a second simultaneous media start;
- pause with bounded in-flight growth, followed by resume and new delivery;
- idempotent media stop;
- media-direction observer callbacks;
- shutdown after an injected RTP transport failure while its peer stream and
  transport are still active; and
- complete runtime teardown with no reported live backend resources.

The PJMEDIA Phase 8 socket-free regression separately passes RTP sequence
wrap, loss, reorder, duplicate, SSRC and malformed/bounds cases, plus RTCP and
jitter-buffer edge cases. The Phase 12 robustness profile separately passes a
120-frame bidirectional UDP stream, repeated setup, active shutdown paths,
malformed SDP, timeout handling, callback checks, and resource checks.

## Build and runtime

Pristine cache-disabled build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase6 -- \
  -DEXTRA_CONF_FILE=phase6_media.conf
```

Final image footprint:

```text
FLASH: 217148 B / 4 MB
RAM:  1149704 B / 4 MB
```

Bounded run:

```sh
timeout --signal=TERM --kill-after=3s 30s \
west build -d build-voip-integration-phase6 -t run
```

The pass marker is emitted before QEMU idles; timeout status 124 is expected
after successful completion.

## Configuration and link boundary

The validated profile enables PJMEDIA endpoint, G.711, RTP/RTCP, jitter
buffer/stream, loop support required by the port, UDP media transport, and
Zephyr network UDP. The shared backend retains its mandatory PJSIP TCP
listener, while SIP UDP, registration, INVITE call control, and audio-device
support are disabled in this focused profile. The final ELF has zero unresolved
symbols.

No upstream PJPROJECT protocol source was modified.

## Regression

The expanded facade was checked against both adjacent integration profiles:

```text
VOIP INTEGRATION PHASE 1 RESULT: PASSED (5 facade lifecycles)
VOIP INTEGRATION PHASE 5 RESULT: PASSED (3 call lifecycles)
```

The Phase 5 rebuild footprint is 255928 B FLASH and 642088 B RAM.

## Remaining limitations

- One account, one call, and one active media session are intentional initial
  limits.
- Phase 6 validates media independently of an INVITE dialog. Binding the
  negotiated Phase 5 SDP and call lifecycle to this backend is the next
  integration phase.
- The sink is validation-only and retains no more than eight decoded frames.
- DNS, TLS, redirects, and SIP UDP are outside this profile.
- Physical ADC/eDMA capture and SPI DAC playback remain the eventual
  MIMXRT1060 hardware integration path.
