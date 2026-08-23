# PJMEDIA Phase 8 RTP/RTCP/Jitter-Buffer Validation

Date: 2026-08-23

## Result

Phase 8 passes on `mps2/an385` under QEMU:

```text
PHASE 8 RESULT: PASSED (3 socket-free RTP/RTCP/jitter lifecycles)
```

This phase validates RTP, RTCP, RTCP feedback, and adaptive jitter-buffer
packet primitives without creating a socket, media transport, media stream,
audio device, PJSIP endpoint, or codec. Phase 9 was not started.

The production changes only activate existing PJPROJECT sources through the
default-off `PJMEDIA_RTP_RTCP` Kconfig gate and the Zephyr module CMake file.
The PJPROJECT root/native CMake file and PJPROJECT C sources are unchanged.

## Environment and entry gate

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Entry revision | `123c36289` (`pjmedia phase 7`) |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Compiler | GCC 14.3.0 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Ninja | 1.10.1 |
| Board | `mps2/an385` |
| Entry free space | 2.9 GiB |

Environment discovery used:

```sh
pwd
sed -n '1,240p' AGENTS.md
git status --short
sed -n '1,24p' pjproject/version.mak
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH west --version
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH west topdir
/home/pitpapan/zephyrproject/.venv/bin/python --version
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH cmake --version
ninja --version
df -h .
```

No file under the Zephyr source directory was inspected or modified.

## Selected closure

The gate adds exactly:

```text
rtp.c
rtcp.c
rtcp_fb.c
jbuf.c
```

`rtcp_fb.c` is required because `rtcp.c` directly invokes its feedback
parsers while processing compound RTCP. Its video paths compile out under the
established `PJMEDIA_HAS_VIDEO=0` profile. Its SDP-facing public functions
resolve using already validated SDP, endpoint, codec-manager, and type helpers;
they introduce no stream or transport implementation.

## Final runtime build and execution

The final pristine build command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase8 -- -DEXTRA_CONF_FILE=phase8_packet.conf
```

Result:

```text
FLASH: 105044 B / 4 MB
RAM:   190056 B / 4 MB
```

The runtime command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase8 -t run
```

The pass marker appeared before timeout returned 124 and stopped the idle
QEMU process.

Each of three complete PJLIB/pool/session lifecycles validates:

- RTP header encode/decode and deterministic sequence/timestamp wrap;
- CSRC and extension-length bounds, truncated headers, and wrong version;
- payload-type rejection, learned SSRC change, duplicate, reordering, gaps,
  late packets, sequence restart, and 16-bit sequence wrap;
- RTCP sender and receiver report construction and parsing;
- exact packet/byte, loss, duplicate, reorder, sender loss, and converted
  jitter statistics;
- RTCP SDES and BYE construction, small output buffers, feedback NACK
  construction/parsing, and truncated/oversized compound packet rejection;
- jitter-buffer fixed and adaptive modes, prefetch, underflow, missing and
  reordered frames, extended sequence wrap, overflow discard, oversized-frame
  truncation, peek/remove, minimum delay, reset, and destruction;
- fixed-point configuration, a bounded RTP sequence-update diagnostic, stack
  watermark, pool peak, and zero tracked allocation after teardown.

The deterministic jitter check feeds a report containing 16 samples at 8 kHz
and verifies the exact 2000-microsecond result. It does not use QEMU scheduling
timing as a pass criterion. The 200,000-call sequence-update diagnostic
reported 10 ms in the final run and 20 ms in an earlier run, so it is retained
only as a target-relative CPU observation.

Runtime resources:

| Measurement | Result |
| --- | ---: |
| PJ allocation peak | 1 block / 16384 B |
| Test pool used peak | 420 B |
| Jitter-buffer capacity reached | 4 frames |
| Main stack | at most 6656 / 24576 B used |
| Main stack unused | 17920 B |

## Public-API link probe

The probe command was:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase8-link-probe -- \
  -DEXTRA_CONF_FILE=phase8_link_probe.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=2s 5s \
  west build -d build-pjmedia-phase8-link-probe -t run
```

It passed at 95012 bytes flash and 190648 bytes RAM and emitted:

```text
PHASE 8 LINK PROBE: PASSED (RTP/RTCP/jitter public closure retained)
```

The probe retains all supported public RTP, RTCP, RTCP-feedback, and
jitter-buffer calls under normal `-ffunction-sections`, `-fdata-sections`, and
`--gc-sections` behavior. Its final ELF has no undefined symbols and contains
57 globally defined `pjmedia_rtp*`, `pjmedia_rtcp*`, or `pjmedia_jbuf*`
symbols.

## Feature-disabled proof and four-object delta

The like-for-like disabled profile differs by setting
`CONFIG_PJMEDIA_RTP_RTCP=n` and selecting only its marker:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase8-disabled -- \
  -DEXTRA_CONF_FILE=phase8_disabled.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=2s 5s \
  west build -d build-pjmedia-phase8-disabled -t run
```

Result:

```text
FLASH: 76076 B
RAM:   188696 B
PHASE 8 DISABLED: PASSED (no RTP/RTCP/jitter objects)
```

The disabled archive contains exactly the earlier eight objects:

```text
errno.c sdp.c sdp_cmp.c endpoint.c codec.c event.c format.c types.c
```

The Phase 8 runtime adds 28968 bytes flash and 1360 bytes static RAM relative
to this configuration-matched image. Runtime PJ memory for its four-frame
jitter buffer is included separately in the measured pool peak.

The disabled overlay initially mirrored the Phase 7 runtime and was built and
run before being refined into the configuration-matched profile above. That
transient build reproduced the exact Phase 7 footprint and pass marker, so it
also supplies the previous-phase regression evidence described below.

## Archive, source, and final-link audit

Commands:

```sh
PJSDK_BIN=/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin
"$PJSDK_BIN/arm-zephyr-eabi-ar" t \
  build-pjmedia-phase8/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-ar" t \
  build-pjmedia-phase8-disabled/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase8/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase8/zephyr/zephyr.elf
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase8-link-probe/zephyr/zephyr.elf
rg -n '^CONFIG_(PJMEDIA|PJSIP)' build-pjmedia-phase8/zephyr/.config
rg -n '/pjproject/.*(transport|stream|pjmedia-audiodev|pjnath|pjsua|pjsip-simple)' \
  build-pjmedia-phase8/build.ninja || true
```

The enabled archive contains exactly 12 objects: the disabled profile's eight
plus the four Phase 8 objects. Archive undefined references classify as
same-archive PJMEDIA calls, validated PJLIB/PJLIB-util/libc calls, or the
compiler helpers `__aeabi_uldivmod` and `__aeabi_ldivmod`. Both final ELFs
resolve every symbol.

No PJMEDIA transport, stream, audio-device, PJNATH, PJSUA, PJSIP-SIMPLE,
third-party codec, video, SRTP, ICE, or RTCP-XR implementation is selected.

## Previous-phase regression

Before refinement into the like-for-like disabled profile, the pristine
`build-pjmedia-phase8-disabled` build used the committed Phase 7 configuration.
It passed at the exact prior footprint:

```text
FLASH: 164932 B
RAM:   386224 B
PHASE 7 RESULT: PASSED (3 endpoint/G.711 lifecycles; PCMU+PCMA)
```

Phase 8 does not create an endpoint, socket, timer, transport, or ioqueue, so
the procedure's additional PJSIP signaling regression is not applicable.

## Development failures and corrections

The first runtime build passed at 105136 bytes flash and 190056 bytes RAM, but
the first execution stopped at an SSRC assertion. PJPROJECT 2.16 learns an
unlocked changed SSRC, while its subsequent sequence update overwrites the
transient `badssrc` status bit. The harness was corrected to validate the
lasting peer-SSRC state. No PJPROJECT source was changed.

A second pristine build passed at 105140 bytes flash and 190056 bytes RAM.
Its runtime aborted after the test directly called the RTCP-feedback parser
with an eight-byte buffer. That API specifies a complete feedback header as a
`PJ_ASSERT_RETURN` precondition, so the call violated the execution procedure.
The malformed case was moved to the outer compound RTCP parser, the correct
validation boundary. No PJPROJECT source was changed.

The corrected incremental build passed at 105044 bytes flash and 190056 bytes
RAM, and all three lifecycles passed. The final pristine build reproduced that
result.

## Completion gates

- [x] RTP encode/decode, wrap, loss, reorder, duplicate, restart, PT, and SSRC
  cases pass deterministically.
- [x] Truncated header and extension bounds are safely rejected.
- [x] RTCP SR/RR and exact loss/jitter statistics pass.
- [x] Compound RTCP and feedback malformed lengths do not corrupt the next
  session.
- [x] Jitter-buffer prefetch, underflow, overflow, discard, reset, missing,
  reorder, and extended wrap pass.
- [x] Three complete session/pool lifecycles leave no tracked allocation.
- [x] Fixed-point profile, CPU diagnostic, resource peak, and stack watermark
  are recorded.
- [x] The public-API closure links under normal garbage collection.
- [x] The feature-disabled archive contains no Phase 8 object.
- [x] Phase 7 still passes.
- [x] Native/root PJPROJECT CMake and PJPROJECT C sources are unchanged.
- [x] No Phase 9 implementation is linked or started.
