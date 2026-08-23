# PJMEDIA Phase 7 Endpoint and G.711 Validation

Date: 2026-08-23

## Result

Phase 7 passes on `mps2/an385` under QEMU:

```text
PHASE 7 RESULT: PASSED (3 endpoint/G.711 lifecycles; PCMU+PCMA)
```

This phase adds the PJMEDIA endpoint and codec-manager runtime and validates
direct G.711 PCMU/PCMA conversion. The endpoint shares the PJSIP endpoint's
ioqueue and creates zero PJMEDIA worker threads. RTP, RTCP, jitter buffering,
media UDP, streams, audio devices, other codecs, PJNATH, and PJSUA remain out
of scope. Phase 8 was not started.

The only production change is conditional source selection in
`pjproject/zephyr/CMakeLists.txt`. The native/root PJPROJECT CMake file and
PJPROJECT C sources are unchanged.

## Environment

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Entry revision | `a9681af90` (`pjmedia phase 6`) |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Compiler | GCC 14.3.0 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Ninja | 1.10.1 |
| Board | `mps2/an385` |

Every `west` command used:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1
```

No file under the Zephyr source directory was inspected or modified.

## Runtime profile

The pristine build command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase7 -- -DEXTRA_CONF_FILE=phase7_g711.conf
```

The runtime command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase7 -t run
```

The marker appeared before `timeout` returned 124 and stopped QEMU. This is
the expected result because the sample idles after completing its tests.

Each of three complete lifecycles validates:

- PJLIB, PJSIP endpoint, PJMEDIA endpoint, and G.711 creation and teardown;
- reuse of the PJSIP ioqueue with zero PJMEDIA workers;
- exactly PCMU payload type 0 and PCMA payload type 8 enumeration;
- codec allocation, open, modify, close, and deallocation;
- fixed encode and decode vectors for both laws;
- short encode buffers, truncated input, invalid payload types, and invalid
  endpoint flags;
- PLC recovery with PLC enabled and rejection with it disabled;
- VAD silence suppression enabled and disabled;
- audio SDP creation, an endpoint child pool, and the threadless event manager;
- G.711 deinitialization before endpoint destruction, one endpoint-exit
  callback, and an empty caching pool after shutdown.

The algorithmic conversion diagnostic executed 50,000,000 calls per law. The
final pristine run reported 1110 ms for u-law and 90 ms for A-law; earlier
runs reported materially different values. It is retained only to prove that
both algorithmic paths execute and to expose their code size. QEMU wall-time
results are not used as a comparative performance claim or a pass criterion.

The measured runtime footprint is 164932 bytes flash and 386224 bytes static
RAM. PJ allocations peaked at 9 blocks / 66160 bytes; the test pool peaked at
1936 bytes used. The main thread used at most 5344 of 32768 bytes, leaving
27424 bytes unused.

## Link-closure profile

The public-API link probe was built pristine and run with:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase7-link-probe -- \
  -DEXTRA_CONF_FILE=phase7_link_probe.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=2s 5s \
  west build -d build-pjmedia-phase7-link-probe -t run
```

It passed at 113004 bytes flash and 370416 bytes RAM and emitted:

```text
PHASE 7 LINK PROBE: PASSED (endpoint/G.711 public closure retained)
```

Representative retained APIs include endpoint, codec manager, G.711,
A-law/u-law, event, audio-format, port, PLC, WSOLA, and silence-detector
families. The final ELF has no undefined symbols.

## Feature-disabled profile

The disabled profile was built pristine with:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase7-disabled -- \
  -DEXTRA_CONF_FILE=phase7_disabled.conf
```

The first attempt failed because the inherited Phase 2 harness calls
`k_thread_stack_space_get()` but its required `CONFIG_INIT_STACKS` and
`CONFIG_THREAD_STACK_INFO` settings were absent. Adding those two application
configuration settings fixed the profile; no PJPROJECT change was involved.

The final build passed at 110304 bytes flash and 194024 bytes RAM. Runtime
emitted:

```text
PHASE 2 RESULT: PASSED (3 complete SDP lifecycles)
```

Its PJMEDIA archive contains exactly `errno.c`, `sdp.c`, and `sdp_cmp.c`; it
contains no Phase 7 endpoint, codec-manager support, or G.711 object.

## Source, archive, and symbol audit

The audit commands were:

```sh
PJSDK_BIN=/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/arm-zephyr-eabi/bin
"$PJSDK_BIN/ar" t build-pjmedia-phase7/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/ar" t build-pjmedia-phase7-disabled/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/nm" -u build-pjmedia-phase7/zephyr/zephyr.elf
"$PJSDK_BIN/nm" -u build-pjmedia-phase7/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/nm" --defined-only \
  build-pjmedia-phase7-link-probe/zephyr/zephyr.elf
rg -n '^CONFIG_(PJMEDIA|PJSIP)' build-pjmedia-phase7/zephyr/.config
rg -n 'rtp\.c|rtcp|jbuf|transport_udp\.c|stream\.c|pjmedia-audiodev|pjnath|pjsua|pjsip-simple' \
  build-pjmedia-phase7/build.ninja || true
```

The enabled PJMEDIA archive contains exactly these 14 source objects:

```text
errno.c sdp.c sdp_cmp.c endpoint.c codec.c event.c format.c types.c port.c
g711.c alaw_ulaw.c plc_common.c wsola.c silencedet.c
```

The archive's undefined symbols classify as expected PJLIB, PJLIB-util,
libc, or same-archive PJMEDIA dependencies. WSOLA also introduces the compiler
runtime helpers `__aeabi_d2uiz`, `__aeabi_dmul`, `__aeabi_i2d`, and
`__aeabi_ui2d` because upstream sizing expressions use double constants even
in the fixed-point profile. The final ELF resolves all of them.

Algorithmic conversion function sizes in the final ELF are 36 bytes for
u-law decode, 42 bytes for A-law decode, 76 bytes for u-law encode, and 88
bytes for A-law encode. No lookup-table conversion source is selected.

No RTP, RTCP, feedback, jitter-buffer, media-UDP, stream, audio-device,
third-party codec, PJNATH, PJSUA, or PJSIP-SIMPLE implementation is linked.

## Regression and resource comparison

Phase 6 was rebuilt pristine with its original command and overlay, then run:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase6 -- -DEXTRA_CONF_FILE=phase6_udp_call.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase6 -t run
```

It retained its prior 259096-byte flash and 737656-byte RAM footprint and
emitted its three-lifecycle pass marker. Phase 7 is 94164 bytes smaller in
flash and 351432 bytes smaller in static RAM than that network-heavy profile,
but the configurations have different heap and network ceilings; this is not
an isolated G.711 cost measurement.

Against the feature-disabled profile, Phase 7 adds 54628 bytes flash and
192200 bytes RAM. That RAM delta includes a 131072-byte configured heap
difference and PJSIP endpoint/configuration differences.

## Development corrections

The first runtime used PLC/WSOLA while checking the exact decoded vector.
Because PLC legitimately conditions the first decoded frame, that check
failed. The harness was corrected to test the pure vector with PLC disabled
and PLC recovery separately with PLC enabled. No library implementation was
changed.

Short benchmark attempts using PJ timestamps and cycle counters were too
coarse on this QEMU target. The final diagnostic uses `k_uptime_get()` and a
larger fixed iteration count. These diagnostics did not alter pass criteria.

## Completion gates

- [x] Three endpoint and direct-G.711 lifecycles pass.
- [x] Deterministic PCMU and PCMA vectors and error paths pass.
- [x] PLC and VAD enabled/disabled behavior passes.
- [x] Exactly PCMU and PCMA are advertised.
- [x] Endpoint shares the signaling ioqueue and uses zero media workers.
- [x] G.711 is destroyed before the endpoint; pools are empty at shutdown.
- [x] Public Phase 7 API closure links under normal garbage collection.
- [x] The feature-disabled profile contains no Phase 7 object.
- [x] The final ELF has no undefined symbol.
- [x] Phase 6 still passes.
- [x] Native/root PJPROJECT CMake and PJPROJECT C sources are unchanged.
- [x] No later-phase media implementation is linked.
- [x] Phase 8 was not started.
