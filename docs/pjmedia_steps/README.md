# PJMEDIA Step-by-Step Guides

These are the short, implementation-oriented guides for working through the
PJMEDIA port one phase at a time. They complement the formal roadmap and audit
procedure:

- [`../PJMEDIA_ZEPHYR_PORT_PLAN.md`](../PJMEDIA_ZEPHYR_PORT_PLAN.md) defines
  scope and completion criteria;
- [`../PJMEDIA_ZEPHYR_PORT_PROCEDURE.md`](../PJMEDIA_ZEPHYR_PORT_PROCEDURE.md)
  defines the complete audit discipline;
- this directory explains where to start and what to do next.

Current validated state: Phase 8 is complete. Phase 9 has not been started.

## Guides

1. [`PHASE4_INVITE_LIFECYCLE.md`](PHASE4_INVITE_LIFECYCLE.md) — add, initialize,
   and destroy PJSIP INVITE support without sending a call.
2. [`PHASE5_LOOP_CALL_CONTROL.md`](PHASE5_LOOP_CALL_CONTROL.md) — complete the
   INVITE state machine using the socket-free PJSIP loop transport.
3. [`PHASE6_UDP_CALL_CONTROL.md`](PHASE6_UDP_CALL_CONTROL.md) — repeat call
   control over deterministic IPv4 UDP loopback while RTP remains disabled.
4. [`PHASE7_ENDPOINT_G711.md`](PHASE7_ENDPOINT_G711.md) — create the PJMEDIA
   endpoint and validate direct PCMU/PCMA conversion without media transport.
5. [`PHASE8_RTP_RTCP_JBUF.md`](PHASE8_RTP_RTCP_JBUF.md) — validate RTP, RTCP,
   feedback, and jitter-buffer primitives without sockets or streams.

Read only the guide for the active phase. Do not prepare files for the next
guide until the active phase has passed and has a validation report.

## Common environment

Run commands from `/home/pitpapan/zephyrproject`:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
```

The development target for these three phases is `mps2/an385` under QEMU.

## What these phases do not prove

Phases 4–8 validate SIP call control, SDP negotiation, the PJMEDIA endpoint,
direct G.711 conversion, and socket-free RTP/RTCP/jitter primitives. They do
not validate media transport callbacks or sockets, an audio stream, or an
audio device. Those begin in Phase 9 or later.
