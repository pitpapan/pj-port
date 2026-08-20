# Phase 6 — INVITE Call Control over IPv4 UDP

## Goal in plain language

Repeat the Phase 5 SIP call-control behavior over real Zephyr IPv4 UDP
loopback sockets while media remains inactive.

Passing Phase 6 proves that INVITE call control works over UDP alongside the
existing registration and OPTIONS functionality. It does not send RTP and
does not prove audio flow.

## Prerequisite

Phase 5 must pass all socket-free call scenarios and clean teardown. Keep its
test available as the regression baseline.

## Production source changes

Normally none.

The PJSIP IPv4 UDP source is already behind `CONFIG_PJSIP_UDP_TRANSPORT` in
`pjproject/zephyr/CMakeLists.txt`. Phase 6 enables that existing selector. Do
not add PJMEDIA `transport_udp.c`; that file is an RTP/RTCP media transport and
belongs to Phase 9.

## Files to change

```text
applications/pjmedia_minimal/Kconfig
applications/pjmedia_minimal/CMakeLists.txt
applications/pjmedia_minimal/src/main.c
applications/pjmedia_minimal/phase6_udp_call.conf              new
applications/pjmedia_minimal/src/phase6_udp_call.c              new
```

Use these existing sources as references without modifying them:

```text
applications/pjsip_minimal/src/phase7_udp.c
applications/pjsip_minimal/src/phase10_signaling.c
applications/pjsip_minimal/src/phase11_robustness.c
```

## Step 1 — Add the validation selector and overlay

Add `PJMEDIA_PHASE6_UDP_CALL_TEST`. It depends on:

```text
PJSIP_INVITE
PJSIP_UDP_TRANSPORT
PJMEDIA_SDP_NEG
NET_SOCKETS
NET_IPV4
NET_UDP
```

Create `phase6_udp_call.conf` from Phase 5, then enable UDP and deterministic
loopback networking:

```ini
CONFIG_PJLIB_UTIL=y
CONFIG_PJMEDIA=y
CONFIG_PJMEDIA_SDP=y
CONFIG_PJMEDIA_SDP_NEG=y
CONFIG_PJSIP=y
CONFIG_PJSIP_INVITE=y
CONFIG_PJSIP_UDP_TRANSPORT=y
CONFIG_PJSIP_TCP_TRANSPORT=n

CONFIG_NET_IPV6=n
CONFIG_NET_LOOPBACK=y

CONFIG_PJMEDIA_ENDPOINT=n
CONFIG_PJMEDIA_G711=n
CONFIG_PJMEDIA_RTP_RTCP=n
CONFIG_PJMEDIA_UDP_TRANSPORT=n
CONFIG_PJMEDIA_STREAM=n
CONFIG_PJMEDIA_AUDIODEV=n

CONFIG_PJMEDIA_PHASE5_LOOP_CALL_TEST=n
CONFIG_PJMEDIA_PHASE6_UDP_CALL_TEST=y

CONFIG_NET_MAX_CONTEXTS=40
CONFIG_NET_MAX_CONN=40
CONFIG_ZVFS_OPEN_MAX=48
CONFIG_ZVFS_POLL_MAX=40
CONFIG_NET_BUF_DATA_SIZE=256
CONFIG_NET_BUF_RX_COUNT=32
CONFIG_NET_BUF_TX_COUNT=32
CONFIG_NET_PKT_RX_COUNT=16
CONFIG_NET_PKT_TX_COUNT=16

CONFIG_HEAP_MEM_POOL_SIZE=524288
CONFIG_MAIN_STACK_SIZE=32768
CONFIG_DYNAMIC_THREAD_POOL_SIZE=8
CONFIG_DYNAMIC_THREAD_STACK_SIZE=8192
CONFIG_INIT_STACKS=y
CONFIG_THREAD_STACK_INFO=y
```

These are validation ceilings based on the existing PJSIP UDP harness. Record
actual usage and reduce only after tests pass.

## Step 2 — First checkpoint: two UDP transports

Reuse the Phase 5 endpoint and module initialization. Before creating an
INVITE:

1. initialize `127.0.0.1` with `pj_sockaddr_in_init()`;
2. start one UAS UDP transport on an ephemeral port;
3. start one UAC UDP transport on another ephemeral port;
4. verify both addresses are IPv4 loopback and both ports are nonzero and
   distinct;
5. install bounded transport state/drop callbacks;
6. start the endpoint event pump;
7. exchange one existing OPTIONS-style request;
8. shut everything down and verify no socket, callback, or pool remains.

Reference:

```text
applications/pjsip_minimal/src/phase7_udp.c
```

Build and run this checkpoint:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase6 -- -DEXTRA_CONF_FILE=phase6_udp_call.conf

timeout --signal=TERM --kill-after=5s 45s \
  west build -d build-pjmedia-phase6 -t run
```

## Step 3 — Move the successful Phase 5 call to UDP

Keep the Phase 5 state/callback assertions. Change only transport and routing:

1. give UAC and UAS explicit SIP URIs containing their loopback UDP ports;
2. give each dialog an explicit Contact URI;
3. bind the UAC dialog or transmit request to the UAC UDP transport;
4. route the request to the UAS UDP port;
5. complete INVITE, 100, 180, 200, ACK, BYE, and 200;
6. verify Via, Contact, dialog target, and received transport information use
   the expected addresses and ports;
7. verify negotiated SDP remains inactive from a media-runtime perspective.

Do not allocate RTP ports. The SDP body is for offer/answer and call-control
validation only.

## Step 4 — Add UDP failure cases

Add one case at a time:

1. CANCEL before final response;
2. transaction timeout to an unused loopback UDP port;
3. one deliberately dropped SIP datagram using the existing PJSIP transport
   manager drop-data callback pattern;
4. retransmission followed by successful delivery;
5. malformed SDP in an incoming request;
6. malformed SDP in a response;
7. UAC transport shutdown during early dialog state;
8. UAS transport shutdown during confirmed state;
9. remote BYE and local BYE;
10. repeated call startup and teardown.

Use callback/state counters and bounded waits. Do not rely on sleeping for a
fixed time and assuming a transition happened.

## Step 5 — Registration and OPTIONS concurrency

After one UDP call is stable, reuse the behavior validated by:

```text
applications/pjsip_minimal/src/phase10_signaling.c
```

During one call lifecycle:

- keep one local registration active;
- exchange at least one OPTIONS transaction;
- complete one INVITE dialog;
- record simultaneous transactions, timers, transports, sockets, and ioqueue
  handles;
- unregister only after the call has terminated.

This is the test that proves Phase 6 did not make existing signaling unusable.

## Step 6 — Teardown order

1. prevent new registration, OPTIONS, and call operations;
2. terminate active INVITE sessions and dialogs;
3. unregister the registration client;
4. drain transactions and timers;
5. shut down UAC and UAS UDP transports;
6. restore transport-manager callbacks;
7. stop and join the event pump;
8. destroy the endpoint;
9. release pools and destroy the caching pool;
10. call `pj_shutdown()`;
11. verify zero late callbacks, sockets, transports, and PJ allocations.

Repeat at least three complete call lifecycles.

## Step 7 — Resource comparison

Record Phase 6 values and deltas from Phase 5:

- flash and static RAM;
- configured and peak heap/PJ allocation;
- main and event-thread stack use;
- live/peak transactions and timers;
- PJSIP transports;
- UDP sockets and ioqueue handles;
- Zephyr network contexts/connections;
- open and poll descriptors;
- late callbacks after transport shutdown.

## Step 8 — Required marker and audits

Final marker:

```text
PHASE 6 RESULT: PASSED (3 complete IPv4 UDP call lifecycles)
```

Audit that:

- `sip_transport_udp.c` is the only newly enabled production transport source;
- PJMEDIA `transport_udp.c`, RTP, RTCP, endpoint, codec implementation, stream,
  audio, PJNATH, PJSUA, and PJSIP-SIMPLE sources remain absent;
- the PJMEDIA archive remains the Phase 3 seven-object closure;
- final ELF undefined symbols are empty;
- Phase 5 still builds with `CONFIG_PJSIP_UDP_TRANSPORT=n`;
- no QEMU process remains.

## Step 9 — Regressions

Rerun:

- Phase 5 loop call control;
- Phase 4 INVITE lifecycle;
- Phase 3 SDP negotiation;
- existing registration/OPTIONS validation if it was not directly integrated
  into the Phase 6 harness.

## Done checklist

- [ ] Successful call control passes over two IPv4 UDP loopback transports.
- [ ] CANCEL, timeout, retransmission, malformed SDP, and peer-close cases pass.
- [ ] Registration and OPTIONS remain usable during a call.
- [ ] Resource delta from Phase 5 is recorded.
- [ ] Shutdown leaves no dialog, transaction, timer, transport, socket,
      callback, thread, or PJ allocation live.
- [ ] No PJMEDIA RTP/media UDP/audio source is linked.
- [ ] Result is described as UDP call control, not media flow.
- [ ] Phase 7 was not started.
