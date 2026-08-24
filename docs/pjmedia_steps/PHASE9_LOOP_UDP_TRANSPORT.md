# Phase 9: PJMEDIA Loop and IPv4 UDP Transports

## Objective

Validate PJMEDIA transport attachment, packet callbacks, explicit IPv4
RTP/RTCP sockets, and teardown before creating an audio stream.

Two default-off production gates define the closure:

- `CONFIG_PJMEDIA_LOOP_TRANSPORT` adds `transport_loop.c` and depends only on
  the Phase 8 packet primitives;
- `CONFIG_PJMEDIA_UDP_TRANSPORT` adds `transport_udp.c` and additionally
  depends on IPv4 UDP sockets. It selects the loop gate so the socket-free
  transport is always validated first.

Phase 9 deliberately excludes media streams, codecs, audio devices, ICE,
SRTP, SDP negotiation, and INVITE handling.

## Build and run

From `/home/pitpapan/zephyrproject`:

```sh
source .venv/bin/activate
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase9 -- -DEXTRA_CONF_FILE=phase9_transport.conf
timeout --signal=TERM --kill-after=5s 25s \
  west build -d build-pjmedia-phase9 -t run
```

Required marker:

```text
PHASE 9 RESULT: PASSED (3 loop/UDP media transport lifecycles)
```

After the marker, timeout status 124 is expected because the sample idles.

Repeat pristine builds with `phase9_link_probe.conf` and
`phase9_disabled.conf` in matching build directories. Require:

```text
PHASE 9 LINK PROBE: PASSED (loop/UDP transport public closure retained)
PHASE 9 DISABLED: PASSED (no media transport objects)
```

## Runtime checklist

- Run the loop transport first: legacy and current attach APIs, two callback
  owners, fanout, receive disable, detach, and close.
- Create two UDP media transports from four explicitly bound
  `127.0.0.1:0` sockets.
- Share the PJSIP endpoint ioqueue with a zero-worker PJMEDIA endpoint.
- Keep a local UDP SIP registration active while RTP/RTCP sockets run.
- Exchange deterministic RTP and RTCP and verify the RTCP source port.
- Exercise RTCP mux separately by routing RTCP to the peer RTP socket.
- Reject a malformed datagram at the callback boundary and an oversized
  datagram at the socket boundary without violating a public API precondition.
- Stop, detach, close, and wait while the shared event thread remains active;
  require no late callback.
- Record the live ioqueue occupancy, the first socket/context capacity
  failure, recovery after release, and both thread stack watermarks.
- Repeat the complete lifecycle three times.

UDP has no connection-level peer-close event. For this phase, peer shutdown is
validated as destruction of the peer media transport followed by local
receive cancellation and callback quiescence. Connected-stream shutdown is a
Phase 10 concern.

## Audit checklist

- Enabled archive: the 12 Phase 8 objects plus exactly `transport_loop.c` and
  `transport_udp.c`.
- Disabled archive: exactly the 12 Phase 8 objects.
- Runtime and probe ELFs: no undefined symbols.
- Probe ELF: all eight supported loop/UDP construction entry points retained.
- Build graph: no stream, audio-device, ICE, SRTP, PJNATH, PJSUA, video, or
  third-party codec implementation.
- Runtime: three lifecycles, registration/unregistration, capacity recovery,
  callback quiescence, stack watermarks, and clean pool teardown.
- Phase 8: pristine rebuild and rerun unchanged.

Do not add `stream.c`; that belongs to Phase 10.
