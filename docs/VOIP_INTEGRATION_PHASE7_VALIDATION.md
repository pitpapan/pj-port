# VoIP Integration Phase 7 SIP-Controlled Media Validation

Date: 2026-08-24

## Result

Phase 7 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 7 RESULT: PASSED (1 complete boot lifecycle)
```

The final image passed the complete matrix on three independent QEMU boots.
Each boot creates one shared PJPROJECT runtime and performs repeated registered
outgoing and incoming SIP calls with automatically controlled PJMEDIA streams.

## Integrated behavior

The call owner now prepares its RTP/RTCP sockets before constructing SDP and
advertises the actual bound RTP port. The active remote SDP supplies the peer
port and negotiated PCMU/PT 0 or PCMA/PT 8 codec. Media starts only after both
successful negotiation and confirmed INVITE state.

Call teardown stops and joins generated PCM, destroys streams, and reports the
inactive media callback before sending local BYE/CANCEL or releasing a remote
dialog. The runtime retains one bounded RTP/RTCP transport pair across
sequential calls and recreates only the negotiated streams. Final shutdown
closes those transports before destroying the PJMEDIA endpoint.

The shared PJ caching pool is bounded so released per-call pools cannot grow
without limit. No PJPROJECT type crosses the public facade.

## Validation matrix

Every boot covers:

- TCP REGISTER and unregister around the complete call sequence;
- registered outgoing and incoming calls over SIP TCP;
- generated bidirectional PCMU and PCMA media over UDP RTP;
- copied facade media callbacks and statistics, including nonzero decoded
  sink hashes and bounded eight-frame sink occupancy;
- rejection of an incompatible answer without starting media;
- local CANCEL and remote CANCEL before media starts;
- local and remote BYE while media is active;
- SIP TCP loss in early and confirmed states, followed by registration
  recovery;
- injected RTP transport loss while SIP remains established, followed by a
  clean facade-controlled BYE;
- offered and offerless incoming calls; and
- repeated sequential call/media teardown with no live resources after the
  final unregister and facade shutdown.

The application controls registration, calls, and media observation through
`VoipManager`. The in-process PJSIP peer is validation infrastructure only and
does not expose PJPROJECT through the product facade.

## Build and runtime

Validated build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase7 -- \
  '-DEXTRA_CONF_FILE=phase5_call.conf;phase7_media.conf'
```

Final footprint:

```text
FLASH:  302680 B / 4 MB
RAM:   2337592 B / 4 MB
```

The large QEMU heap is a validation allowance for the local registrar, SIP
peer, production backend, and both ends of the headless media path in one
image. It is not the MIMXRT1060 production memory budget.

The final ELF was run three times with a bounded QEMU command. Each run emitted
the pass marker before QEMU was terminated after completion.

## Regression

After the media lifecycle changes:

```text
VOIP INTEGRATION PHASE 6 RESULT: PASSED (3 media lifecycles)
VOIP INTEGRATION PHASE 5 RESULT: PASSED (3 call lifecycles)
```

Phase 6 rebuilt at 217556 B FLASH and 1149704 B RAM. Phase 5 rebuilt at
256068 B FLASH and 642088 B RAM.

## Boundaries and remaining work

- SIP signaling remains TCP-only and RTP remains UDP-only.
- One account, one call, and one media session remain intentional limits.
- Hold/resume, DTMF, re-INVITE, and partial-media recovery policy belong to
  Phase 8.
- ADC/eDMA capture and SPI DAC playback remain the eventual MIMXRT1060
  hardware path.
- No upstream PJPROJECT protocol source was modified.
