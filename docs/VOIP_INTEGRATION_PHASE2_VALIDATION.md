# VoIP Integration Phase 2 Shared Runtime Validation

Date: 2026-08-24

## Result

Phase 2 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 2 RESULT: PASSED (5 runtime lifecycles)
```

`PjVoipBackend` now owns one shared PJLIB, PJLIB-UTIL, PJSIP, and PJMEDIA
runtime behind the Phase 1 facade.  It creates one PJSIP endpoint/ioqueue, one
zero-worker PJMEDIA endpoint sharing that ioqueue, one event-processing thread,
one bounded command queue, and the mandatory IPv4 TCP transport factory.

Account, registration, dialog, call, codec, RTP, and audio behavior remain
disabled in this phase.  Their facade operations return `invalid_state` until
the corresponding later integration phase implements them.

## Runtime ownership

Initialization order is:

1. `pj_init()`;
2. `pjlib_util_init()`;
3. caching pool factory;
4. PJSIP endpoint and its timer heap/ioqueue/transport manager;
5. PJMEDIA endpoint sharing the PJSIP ioqueue with zero media workers;
6. IPv4 TCP factory bound to an ephemeral loopback port;
7. event-thread pool and one PJLIB-created event thread; and
8. acceptance of cross-thread facade commands.

Shutdown stops command acceptance first, destroys the TCP listener while the
event pump can drain it, stops and joins the event thread, releases its pool,
destroys PJMEDIA before PJSIP, destroys the caching pool, and finally calls
`pj_shutdown()`.

The implementation is in `voip/src/PjVoipBackend.cpp`.  PJPROJECT types remain
inside that implementation; `PjVoipBackend.hpp` uses an opaque implementation
pointer and exposes no PJ header or handle.

## Command and error boundary

The runtime command queue has exactly eight slots.  Public submissions are
nonblocking:

- accepted commands return `Error::ok`;
- a ninth command while processing is paused returns `Error::queue_full`;
- commands after `BeginShutdown()` return `Error::shutting_down`; and
- commands after complete teardown return `Error::not_initialized`.

Phase 2 probe commands execute only on the event thread and record their count
and last value.  Later phases replace/add bounded account and call commands
without changing this ownership rule.  The runtime translates public PJ
success, invalid-argument, capacity, memory, and generic failures into facade
errors; later phases extend the mapping with SIP/registration/media context.

## Failure-injection coverage

Every injected initialization stop returned `internal_failure`, ran the same
reverse-order cleanup path, reported no live owned resource, and allowed the
next PJLIB lifecycle to initialize:

1. after PJLIB;
2. after PJLIB-UTIL;
3. after the caching pool factory;
4. after the PJSIP endpoint;
5. after the PJMEDIA endpoint;
6. after the TCP factory; and
7. after the event thread.

## Build and runtime

Authoritative pristine build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase2 -- \
  -DEXTRA_CONF_FILE=phase2_runtime.conf
```

Image footprint:

```text
FLASH: 165836 B / 4 MB
RAM:   560232 B / 4 MB
```

Authoritative bounded run:

```sh
timeout --signal=TERM --kill-after=3s 8s \
west build -d build-voip-integration-phase2 -t run
```

All seven partial initializations and five full runtime lifecycles passed in
less than one simulated second.  The command returned 124 only after the pass
marker because QEMU idled afterward.

Every full lifecycle proved:

- a nonzero ephemeral IPv4 TCP listener port;
- a running event thread;
- eight queued commands processed in order;
- controlled ninth-command saturation;
- rejection immediately after shutdown begins;
- TCP factory, event thread, media endpoint, SIP endpoint, pools, and PJLIB
  teardown; and
- rejection after destruction.

## Configuration and source boundary

The Phase 2 overlay enables:

```text
PJSIP_TCP_TRANSPORT=y
PJSIP_UDP_TRANSPORT=n
PJMEDIA_ENDPOINT=y
PJMEDIA_G711=n
PJMEDIA_RTP_RTCP=n
PJMEDIA_UDP_TRANSPORT=n
PJMEDIA_STREAM=n
PJMEDIA_AUDIODEV=n
PJSIP_INVITE=n
```

Thus the runtime contains the TCP signaling foundation but no SIP UDP
transport, registration client, INVITE usage, codec implementation, RTP/RTCP,
media transport, stream, or audio device.  No PJPROJECT C source or native
build file was modified.

## Phase 1 regression

The Phase 1 fake-backend profile was rebuilt pristine and passed all five
facade lifecycles at its unchanged footprint:

```text
FLASH: 16108 B
RAM:   21496 B
VOIP INTEGRATION PHASE 1 RESULT: PASSED (5 facade lifecycles)
```

## Remaining limitations

- The TCP factory is created and its transport type validated by construction,
  but Phase 2 does not exchange SIP messages.  Registration and call phases
  must prove actual Via/Contact/route TCP semantics.
- Runtime heap and stack settings retain conservative validated values.  Phase
  9 measures peaks under the integrated traffic profile before reduction.
- The QEMU random generator remains test-only and cannot supply product SIP or
  media identifiers.
- Account configuration and a `pjsip_regc` begin in Phase 3.
