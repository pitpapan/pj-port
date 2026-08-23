# PJMEDIA Phase 5 Loop Call-Control Validation

Date: 2026-08-22

## Result

Phase 5 passes on `mps2/an385` under QEMU:

```text
PHASE 5 RESULT: PASSED (3 complete socket-free call lifecycles)
```

The harness performs SIP call control through PJSIP's in-memory loop-datagram
transport. It does not enable a network transport, RTP, a media stream, codec
implementation, or an audio device. This validates call control, not an audio
call. Phase 6 was not started.

No production PJPROJECT source or production source selection changed in this
phase. All Phase 5 changes are application validation code, configuration,
documentation, and build-directory hygiene.

## Environment

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Workspace revision before Phase 5 | `89e5b72c61c502d474eb77ee5aa2475b2ac03928` |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Compiler | GCC 14.3.0 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Board | `mps2/an385` |
| Build directory | `build-pjmedia-phase5` |

## Phase 5 build and runtime

The pristine build command was:

```sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase5 -- -DEXTRA_CONF_FILE=phase5_loop_call.conf
```

Result: passed. The final footprint was:

```text
FLASH: 237732 B / 4 MB (5.67%)
RAM:   543496 B / 4 MB (12.96%)
```

The runtime command was:

```sh
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase5 -t run
```

The pass marker appeared after approximately 11.31 simulated seconds. QEMU
then remained idle, so `timeout` stopped it and returned status 124 after the
successful marker. A process check confirmed that no QEMU process remained.

Every one of the three complete PJLIB/endpoint/transport lifecycles passed:

- loop transport identity, event-pump, and endpoint-timer checkpoint;
- INVITE, 100, 180, 200, ACK, UAC BYE, and 200;
- the same connected flow with a UAS-initiated BYE;
- CANCEL with 200 for CANCEL and 487 for INVITE;
- 486 and 603 final-response rejection;
- packet discard and transaction timeout;
- immediate and delayed loop-transport failure;
- offerless INVITE with the answer completed by ACK;
- re-INVITE with sendonly/recvonly negotiated direction;
- incompatible G729-only renegotiation rejected with a controlled SIP 500,
  while the established call remained usable and ended normally with BYE.

The incompatible re-INVITE result is intentionally described as a controlled
SIP error, not specifically 488. PJPROJECT 2.16's callback-driven path used by
this harness emits 500 for that failure and preserves the established dialog.

Between scenarios, the harness waits for zero transactions, dialogs, and
endpoint timers. Each lifecycle also shuts down and releases the loop
transport, stops and destroys the event thread, unregisters validation
modules, destroys the endpoint, verifies exactly one endpoint-exit callback,
and verifies an empty caching pool.

## Source and link audit

The commands used for the final audit were:

```sh
PJSDK_BIN=/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/arm-zephyr-eabi/bin
"$PJSDK_BIN/ar" t build-pjmedia-phase5/modules/pjproject/libpjsip.a
"$PJSDK_BIN/ar" t build-pjmedia-phase5/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/nm" -u build-pjmedia-phase5/zephyr/zephyr.elf
rg -n '^CONFIG_(PJSIP_(UDP|TCP)_TRANSPORT|PJMEDIA_(ENDPOINT|G711|RTP_RTCP|UDP_TRANSPORT|STREAM|AUDIODEV))=' \
  build-pjmedia-phase5/zephyr/.config
rg -n 'sip_transport_(udp|tcp)\.c|pjmedia/(endpoint|rtp|rtcp|transport_udp|stream|g711)\.c|pjmedia-audiodev' \
  build-pjmedia-phase5/build.ninja || true
"$PJSDK_BIN/nm" --defined-only build-pjmedia-phase5/zephyr/zephyr.elf | \
  rg 'pjsip_(loop_start|inv_invite|inv_send_msg|inv_reinvite|inv_end_session)|pjmedia_sdp_neg_'
pgrep -af '[q]emu-system-arm' || true
```

Results:

- the PJSIP archive contains the established core and Phase 4 INVITE,
  100rel, and timer objects; Phase 5 adds no production object;
- the PJMEDIA archive remains the Phase 3 seven-object closure:
  `errno.c`, `sdp.c`, `sdp_cmp.c`, `sdp_neg.c`, `codec.c`,
  `stream_common.c`, and `types.c`;
- the undefined-symbol query produced no output;
- no disabled socket/media family was selected and no forbidden production
  source appeared in `build.ninja`;
- the final ELF defines the expected loop-transport, INVITE/re-INVITE,
  end-session, and SDP-negotiation APIs;
- no QEMU process remained.

## Regression commands and results

Phase 4 was rebuilt pristine and run:

```sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase4 -- -DEXTRA_CONF_FILE=phase4_invite.conf
timeout --signal=TERM --kill-after=5s 5s \
  west build -d build-pjmedia-phase4 -t run
```

```text
PHASE 4 RESULT: PASSED (3 complete INVITE module lifecycles)
```

Phase 3 was rebuilt pristine and run:

```sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase3 -- -DEXTRA_CONF_FILE=phase3_sdp_neg.conf
timeout --signal=TERM --kill-after=5s 15s \
  west build -d build-pjmedia-phase3 -t run
```

```text
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

The existing PJSIP Phase 11 image, which also covers the Phase 7 UDP and
Phase 10 registration/OPTIONS lifecycles, was rebuilt pristine and run:

```sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-pjsip-phase11 -- -DEXTRA_CONF_FILE=phase11_robustness.conf
timeout --signal=TERM --kill-after=5s 60s \
  west build -d build-pjsip-phase11 -t run
```

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

## PJPROJECT 2.16 implementation findings

- A newly created UAC dialog must remain caller-locked across
  `pjsip_dlg_set_transport()` and `pjsip_inv_create_uac()`. Until the INVITE
  usage exists, the internal lock release in transport binding can otherwise
  destroy the zero-usage dialog.
- The 100, 180, and final INVITE responses must not all be queued
  back-to-back from one receive callback. Scheduling 180 and the final response
  on separate endpoint timer turns preserves the provisional state when using
  the asynchronous loop transport.
- A synchronous immediate send failure may already disconnect and destroy the
  INVITE session. The failure test therefore observes the disconnect and does
  not dereference that session afterward.
- Loop shutdown must be drained while the endpoint event pump is still active;
  the pump is stopped only after transport transactions and timers quiesce.

## Completion gates

- [x] Three complete socket-free call-control lifecycles pass.
- [x] Successful calls, both BYE directions, CANCEL, rejection, timeout, and
  transport failures pass.
- [x] Offerless INVITE and re-INVITE direction changes pass.
- [x] Incompatible renegotiation produces a controlled SIP error without
  destroying the established call.
- [x] No PJSIP UDP/TCP or PJMEDIA RTP/audio source is linked.
- [x] Phase 3, Phase 4, and existing signaling/resource regressions pass.
- [x] PJPROJECT has no Phase 5 modification.
- [x] No QEMU process remains.
- [x] Phase 6 was not started.
