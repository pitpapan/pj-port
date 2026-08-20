# PJMEDIA Step-by-Step Guides

These are the short, implementation-oriented guides for working through the
PJMEDIA port one phase at a time. They complement the formal roadmap and audit
procedure:

- [`../PJMEDIA_ZEPHYR_PORT_PLAN.md`](../PJMEDIA_ZEPHYR_PORT_PLAN.md) defines
  scope and completion criteria;
- [`../PJMEDIA_ZEPHYR_PORT_PROCEDURE.md`](../PJMEDIA_ZEPHYR_PORT_PROCEDURE.md)
  defines the complete audit discipline;
- this directory explains where to start and what to do next.

Current validated state: Phase 3 is complete. Start with Phase 4.

## Guides

1. [`PHASE4_INVITE_LIFECYCLE.md`](PHASE4_INVITE_LIFECYCLE.md) — add, initialize,
   and destroy PJSIP INVITE support without sending a call.
2. [`PHASE5_LOOP_CALL_CONTROL.md`](PHASE5_LOOP_CALL_CONTROL.md) — complete the
   INVITE state machine using the socket-free PJSIP loop transport.
3. [`PHASE6_UDP_CALL_CONTROL.md`](PHASE6_UDP_CALL_CONTROL.md) — repeat call
   control over deterministic IPv4 UDP loopback while RTP remains disabled.

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

Phases 4–6 validate SIP call control and SDP negotiation. They do not compile
or validate a PJMEDIA endpoint, G.711 implementation, RTP/RTCP, media UDP
transport, audio stream, or audio device. Those begin in Phase 7.
