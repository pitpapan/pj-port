# PJMEDIA Phase 6 IPv4 UDP Call-Control Validation

Date: 2026-08-23

## Result

Phase 6 passes on `mps2/an385` under QEMU:

```text
PHASE 6 RESULT: PASSED (3 complete IPv4 UDP call lifecycles)
```

This phase validates SIP INVITE call control between two distinct PJSIP IPv4
UDP loopback transports. It does not enable PJMEDIA RTP, media UDP, codecs,
streams, audio devices, or send media. Phase 7 was not started.

No PJPROJECT file was modified. The production-source change is selection of
the already ported PJSIP `sip_transport_udp.c` and `sip_reg.c` objects through
the existing Zephyr module configuration. All new executable logic is in the
application validation harness.

## Environment

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Workspace revision before Phase 6 | `8657a3fa7` (`pjmedia phase 5`) |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Compiler | GCC 14.3.0 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Board | `mps2/an385` |
| Build directory | `build-pjmedia-phase6` |

`west` was made available for every command with:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH
```

## Final build and runtime

The final pristine build command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase6 -- -DEXTRA_CONF_FILE=phase6_udp_call.conf
```

Result: passed. The final footprint was:

```text
FLASH: 259096 B / 4 MB (6.18%)
RAM:   737656 B / 4 MB (17.59%)
```

The final runtime command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase6 -t run
```

All three complete lifecycles emitted the pass marker after approximately
13.69 simulated seconds. QEMU then remained idle, so `timeout` stopped it and
returned status 124 after the successful marker. A process check confirmed
that no QEMU process remained.

Each lifecycle validated:

- two different ephemeral `127.0.0.1` UDP transports and explicit SIP routing;
- Via, Contact, target, and received-transport address/port identity;
- INVITE/100/180/200/ACK with UAC-initiated and UAS-initiated BYE;
- CANCEL/200/487 and 486/603 rejection;
- an unused-port transaction timeout;
- deliberate first-INVITE loss, UDP retransmission, and successful recovery;
- parser rejection plus malformed request and response SDP handling;
- offerless INVITE with the answer in ACK;
- re-INVITE hold direction and controlled rejection of incompatible SDP while
  preserving the established call;
- a live registration and OPTIONS exchange during a confirmed call;
- UAC shutdown in early state and UAS shutdown in confirmed state, followed by
  deterministic cleanup and transport restart;
- zero dialogs, transactions, and timers before final teardown, exact bounded
  transport callbacks, one endpoint-exit callback, an empty caching pool, and
  zero tracked PJ pool allocations.

## Resource results and Phase 5 delta

| Measurement | Phase 5 | Phase 6 | Delta |
| --- | ---: | ---: | ---: |
| Flash | 237732 B | 259096 B | +21364 B |
| Static RAM | 543496 B | 737656 B | +194160 B |
| PJ heap peak | not recorded | 171432 B / 59 blocks | n/a |
| Transactions, peak | not recorded | 4 | n/a |
| Endpoint timers, peak | not recorded | 6 | n/a |
| Harness-owned PJSIP UDP transports | 0 | 2 | +2 |
| UDP sockets, peak | 0 | 3 | +3 |
| PJSIP ioqueue handles | 0 | 2 | +2 |

The third socket is the bounded raw-datagram injector used by the malformed
packet test; normal call operation owns the two PJSIP UDP sockets. Configured
network ceilings were 40 contexts, 40 connections, 48 open descriptors, and
40 poll descriptors.

The main-thread watermark reported at most 9808 of 32768 bytes used, leaving
22960 bytes unused. The PJ-created event thread reported zero unused bytes for
both an 8192-byte run and a diagnostic 12288-byte run. Increasing the stack did
not change that result, so this watermark path is not a reliable headroom
measurement for that thread. The final configuration remains 8192 bytes; all
three lifecycles completed without a stack fault, but the result is not used to
justify reducing the stack.

## Source and link closure audit

The commands were:

```sh
PJSDK_BIN=/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/arm-zephyr-eabi/bin
"$PJSDK_BIN/ar" t build-pjmedia-phase6/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/ar" t build-pjmedia-phase6/modules/pjproject/libpjsip.a
"$PJSDK_BIN/nm" -u build-pjmedia-phase6/zephyr/zephyr.elf
rg -n '^CONFIG_(PJSIP_TCP_TRANSPORT|PJMEDIA_(ENDPOINT|G711|RTP_RTCP|UDP_TRANSPORT|STREAM|AUDIODEV))=' \
  build-pjmedia-phase6/zephyr/.config
rg -n 'sip_transport_tcp\.c|pjmedia/(endpoint|rtp|rtcp|transport_udp|stream|g711)\.c|pjmedia-audiodev|pjnath|pjsua|pjsip-simple' \
  build-pjmedia-phase6/build.ninja || true
"$PJSDK_BIN/nm" --defined-only build-pjmedia-phase6/zephyr/zephyr.elf | \
  rg 'pjsip_udp_transport_start$|pjsip_inv_create_uac$|pjsip_regc_create$|pjmedia_sdp_neg_create_w_local_offer$'
pgrep -af '[q]emu-system-arm' || true
```

Results:

- `libpjmedia.a` contains exactly the Phase 3 seven-object closure:
  `errno.c`, `sdp.c`, `sdp_cmp.c`, `sdp_neg.c`, `codec.c`,
  `stream_common.c`, and `types.c`;
- `libpjsip.a` adds `sip_transport_udp.c` for real signaling transport and
  `sip_reg.c` for the required registration concurrency case;
- the final ELF has no undefined symbols and defines the expected UDP,
  INVITE, registration-client, and SDP-negotiation APIs;
- no TCP transport, PJMEDIA endpoint/RTP/RTCP/media-UDP/stream/codec
  implementation/audio, PJNATH, PJSUA, or PJSIP-SIMPLE source was selected;
- no QEMU process remained.

Repository hygiene was checked with:

```sh
git diff --check
git diff --name-only -- pjproject
git diff --quiet -- pjproject
git check-ignore -v build-pjmedia-phase6/zephyr/zephyr.elf
```

Results: the diff check passed, both PJPROJECT checks confirmed no change, and
the Phase 6 build directory is ignored by `.gitignore`.

## Regression commands and results

Phase 5 was rebuilt pristine and run:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase5 -- -DEXTRA_CONF_FILE=phase5_loop_call.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase5 -t run
```

The pristine build passed at 237732 bytes flash and 543496 bytes RAM. The
first runtime attempt exposed an existing timing sensitivity: an initial loop
INVITE retransmitted before the harness's exact-count assertion and that early
abort left two transmit buffers for its cleanup check. No Phase 5 or PJPROJECT
file was changed. An immediate rerun passed all lifecycles:

```text
PHASE 5 RESULT: PASSED (3 complete socket-free call lifecycles)
```

Phase 4 was rebuilt pristine and run with its `phase4_invite.conf` overlay:

```text
FLASH: 220720 B
RAM:   346312 B
PHASE 4 RESULT: PASSED (3 complete INVITE module lifecycles)
```

Phase 3 was rebuilt pristine and run with its `phase3_sdp_neg.conf` overlay:

```text
FLASH: 117272 B
RAM:   263712 B
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

Registration and OPTIONS were tested directly inside every Phase 6 lifecycle,
so no separate signaling rerun was required.

## Completion gates

- [x] Three complete IPv4 UDP call-control lifecycles pass.
- [x] Successful calls, both BYE directions, CANCEL, rejection, timeout,
  retransmission, malformed input, and peer-close cases pass.
- [x] Registration and OPTIONS remain usable during a confirmed call.
- [x] Resource values and the static Phase 5 delta are recorded.
- [x] Teardown leaves no dialog, transaction, timer, transport, callback,
  event thread, or tracked PJ allocation live.
- [x] No PJMEDIA RTP/media UDP/audio source is linked.
- [x] Phase 5, Phase 4, and Phase 3 regressions pass.
- [x] PJPROJECT has no Phase 6 modification.
- [x] No QEMU process remains.
- [x] Phase 7 was not started.
