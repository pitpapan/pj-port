# VoIP Integration Phase 5 One-Call TCP Validation

Date: 2026-08-24

## Result

Phase 5 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 5 RESULT: PASSED (3 call lifecycles)
```

The existing-compatible C++ facade now owns one PJSIP dialog and INVITE
session. It supports outgoing calls, incoming notification, accept, reject,
and end-call operations while an account remains registered. A second call is
rejected with `busy`. No PJSIP or PJMEDIA type is exposed through the facade.

## Production behavior

`CONFIG_VOIP_PJ_CALL_CONTROL` initializes the PJSIP UA, 100rel, timer, INVITE,
and application call-owner modules on the shared Phase 2 endpoint and event
thread. All facade commands remain bounded synchronous queue submissions;
observer callbacks report asynchronous SIP state.

The call owner:

- creates UAC and UAS dialogs and owns exactly one active INVITE session;
- maps calling, incoming, early, confirmed, failed, disconnecting, and
  disconnected state to the existing observer model;
- offers PCMU/PT 0 and PCMA/PT 8 and reports the selected codec;
- accepts offerless INVITEs by supplying its local SDP;
- sends SIP exclusively through the configured TCP listener/transport;
- maps active-call TCP loss to `transport_failure`; and
- restores the transport-manager callback and destroys dialog resources during
  shutdown.

Phase 5 deliberately performs SDP negotiation without creating RTP or media
streams. Media lifecycle integration begins in Phase 6.

## Deterministic call-control matrix

The QEMU application installs a local PJSIP peer and registrar on the
backend's loopback TCP listener. Each of three complete backend lifecycles
proves:

- outgoing INVITE, 100/180 provisional responses, 200 final response, ACK,
  local BYE, and remote BYE;
- incoming INVITE notification, delayed accept, local 486 reject, and the
  one-call busy gate;
- local CANCEL with 200/CANCEL and 487/INVITE, plus remote CANCEL/487;
- offered and offerless incoming INVITEs;
- PCMU negotiation, PCMA-only negotiation, and controlled unsupported-codec
  rejection with SIP 488;
- forced TCP loss in early and confirmed states, observer failure delivery,
  dialog release, reconnect, and successful registration recovery;
- automatic registration refresh while an accepted call is confirmed; and
- expiry-zero unregister followed by endpoint teardown with zero timer entries.

All outgoing INVITEs were counted on `PJSIP_TRANSPORT_TCP`. The test profile
does not build a SIP UDP transport.

## Build and runtime

Validated build command:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase5 -- \
  -DEXTRA_CONF_FILE=phase5_call.conf
```

Final image footprint:

```text
FLASH: 255888 B / 4 MB
RAM:   642088 B / 4 MB
```

Bounded run:

```sh
timeout --signal=TERM --kill-after=3s 90s \
west build -d build-voip-integration-phase5 -t run
```

The pass marker is emitted before QEMU idles; timeout status 124 is therefore
expected after successful completion.

## Configuration and link boundary

The validated profile has call control, PJSIP INVITE/SDP negotiation, SIP TCP,
and network TCP enabled. SIP UDP, network UDP, PJMEDIA G.711, RTP/RTCP,
stream, UDP media transport, and audio-device support are disabled. `nm -u`
reports zero unresolved symbols in the final ELF.

No upstream PJPROJECT protocol source was modified.

## Regression

Phase 4 was rebuilt pristine after the backend changes and all three TCP
Digest/refresh/unregister lifecycles passed:

```text
FLASH: 202192 B / 4 MB
RAM:   560488 B / 4 MB
VOIP INTEGRATION PHASE 4 RESULT: PASSED (3 registration lifecycles)
```

## Remaining limitations

- One configured account and one call are intentional initial limits.
- SDP uses deterministic loopback RTP addresses but no RTP socket is opened in
  this phase.
- DNS, TLS, redirects, and SIP UDP are not enabled by this profile.
- G.711 packetization, generated PCM, the memory sink, and UDP RTP are Phase 6.
- Physical ADC/eDMA capture and SPI DAC playback remain hardware integration
  work for MIMXRT1060.
