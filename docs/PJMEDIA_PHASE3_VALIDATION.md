# PJMEDIA Phase 3 SDP Negotiation Validation

Date: 2026-08-20

## Result

Phase 3 passed under QEMU with PJPROJECT 2.16, Zephyr 4.4.0, west 1.5.0,
Zephyr SDK 1.0.1, GNU Arm Embedded 14.3.0, and the `mps2/an385` board.
Phase 4 was not started.

The exact PJMEDIA archive source closure is:

```text
pjmedia/src/pjmedia/errno.c
pjmedia/src/pjmedia/sdp.c
pjmedia/src/pjmedia/sdp_cmp.c
pjmedia/src/pjmedia/sdp_neg.c
pjmedia/src/pjmedia/codec.c
pjmedia/src/pjmedia/stream_common.c
pjmedia/src/pjmedia/types.c
```

The four Phase 3 additions are selected only by `CONFIG_PJMEDIA_SDP_NEG`.
No PJPROJECT source refactor or stub was needed. The PJPROJECT root/native
`CMakeLists.txt` remains unchanged.

## Test coverage

Each of three complete PJLIB/PJLIB-UTIL/PJMEDIA lifecycles covered:

- local-offer and remote-offer negotiator creation;
- deferred remote-offer answering through the `REMOTE_OFFER` state;
- PCMU and PCMA static-payload matching;
- telephone-event and `fmtp` matching;
- local and remote codec-order preference;
- `sendrecv`, `sendonly`, `recvonly`, and `inactive` direction negotiation;
- rejected media with port zero while another media remains active;
- no-common-codec rejection as `PJMEDIA_SDPNEG_NOANSCODEC`;
- incompatible transport rejection as `PJMEDIA_SDPNEG_EINVANSTP`;
- malformed offer validation and malformed answer parse rejection;
- local offer, remote offer, wait, done, modify, cancel, and renegotiation
  state transitions;
- exact configured limits of four media and sixteen formats per media;
- fifth-media overflow rejection as `PJ_ETOOMANY`;
- repeated negotiator allocation and complete pool teardown.

The negative tests do not intentionally violate a negotiator API precondition,
because PJPROJECT debug builds assert on such misuse. Malformed input is
rejected at its documented parser or validation boundary before it reaches a
state transition.

## Runtime build and execution

Pristine build:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase3 -- -DEXTRA_CONF_FILE=phase3_sdp_neg.conf
```

The build completed all 307 steps. Final footprint:

```text
FLASH: 117272 B / 4 MB (2.80%)
RAM:   263712 B / 4 MB (6.29%)
```

Runtime command:

```sh
timeout --signal=TERM --kill-after=3s 12s \
  west build -d build-pjmedia-phase3 -t run
```

Final markers:

```text
[Phase 3] remote codec order: PASSED
[Phase 3] local codec order: PASSED
[Phase 3] local offer/wait/done/modify/cancel/renegotiate: PASSED
[Phase 3] deferred remote offer/answer and inactive: PASSED
[Phase 3] port-zero/no-codec/transport/malformed rejection: PASSED
[Phase 3] reduced SDP exact-limit and overflow boundary: PASSED
[Phase 3] lifecycle 1 pool teardown: PASSED
[Phase 3] lifecycle 2 pool teardown: PASSED
[Phase 3] lifecycle 3 pool teardown: PASSED
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

The command returned 124 only after the pass marker because the bounded runner
terminated idle QEMU.

## Resource measurements

| Resource | Phase 3 result |
| --- | ---: |
| Zephyr heap | 196,608 B configured |
| PJ pool blocks | 2 blocks / 98,304 B peak |
| PJ pool used size | 71,002 B peak |
| PJ blocks after each teardown | 0 |
| Checked-out pools after each teardown | 0 |
| Main stack | 32,768 B configured |
| Main stack used | at most 14,368 B |
| Main stack unused | 18,400 B |

## Whole-public-API link probe

The additional probe uses a separate application selector and retains calls to
all 22 public functions declared by `pjmedia/sdp_neg.h`. Its build command was:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase3-link-probe \
  -- -DEXTRA_CONF_FILE=phase3_link_probe.conf
```

The normal compile and link policy was active:

```text
-ffunction-sections -fdata-sections
-Wl,--gc-sections
```

The probe initially failed to compile because its call to
`pjmedia_sdp_neg_fmt_match()` used the wrong PJPROJECT 2.16 signature. After
correcting the arguments, it linked. A subsequent audit found that forming an
argument through a compile-time null session let the compiler replace that one
unreachable call with a trap. Passing typed null media pointers instead made
the reference real. The final ELF contains all 22 `pjmedia_sdp_neg_*` public
symbols and has zero undefined symbols.

Runtime command and result:

```sh
timeout --signal=TERM --kill-after=3s 8s \
  west build -d build-pjmedia-phase3-link-probe -t run
```

```text
PHASE 3 LINK PROBE: PASSED (all public negotiator APIs retained)
```

Final probe footprint:

```text
FLASH: 93232 B / 4 MB (2.22%)
RAM:   189512 B / 4 MB (4.52%)
```

## Archive and final-link audit

Commands:

```sh
arm-zephyr-eabi-ar t build-pjmedia-phase3/modules/pjproject/libpjmedia.a
arm-zephyr-eabi-nm -u build-pjmedia-phase3/modules/pjproject/libpjmedia.a
arm-zephyr-eabi-nm -u build-pjmedia-phase3/zephyr/zephyr.elf
arm-zephyr-eabi-nm -u \
  build-pjmedia-phase3-link-probe/zephyr/zephyr.elf
arm-zephyr-eabi-nm -g --defined-only \
  build-pjmedia-phase3-link-probe/zephyr/zephyr.elf | \
  rg " pjmedia_sdp_neg_"
```

The archive contains exactly the seven objects listed above. Its unresolved
references were recorded rather than hidden. They fall into these classes:

- libc (`memcpy`, `memmove`, `memset`, `snprintf`, and string helpers);
- PJLIB and PJLIB-UTIL pool, string, array, scanner, mutex, socket-address,
  logging, group-lock, and exception helpers;
- calls between the seven selected PJMEDIA objects;
- endpoint/RTP/RTCP/jitter/AV-sync references from unrelated functions in
  `stream_common.c`.

The last class remains unresolved inside the static archive by design. The
negotiator needs only `pjmedia_stream_info_parse_fmtp()` from that object. The
whole-public-API probe proves that all supported negotiation APIs link without
retaining those unrelated stream-runtime sections. Both final ELFs have zero
undefined symbols; no unsupported symbol was stubbed.

Source-graph and final-symbol queries found no endpoint, RTP, RTCP, media
transport, stream create/destroy, G.711 implementation, audio-device, video,
SRTP, ICE, PJNATH, PJSIP, or codec-library implementation source or symbol.
`CONFIG_PJMEDIA_ENDPOINT` and `CONFIG_PJSIP` are unset.

## Phase 2 regression

After the CMake change, the Phase 2 build was regenerated and run again:

```sh
west build -d build-pjmedia-phase2
arm-zephyr-eabi-ar t build-pjmedia-phase2/modules/pjproject/libpjmedia.a
timeout --signal=TERM --kill-after=3s 10s \
  west build -d build-pjmedia-phase2 -t run
```

Its archive still contains only `errno.c.obj`, `sdp.c.obj`, and
`sdp_cmp.c.obj`. All three Phase 2 lifecycles passed, confirming that
`CONFIG_PJMEDIA_SDP_NEG=n` excludes the four Phase 3 objects.

## Validation corrections

The following issues were found and corrected during execution:

- invalid-state and inactive-result negative calls triggered PJPROJECT debug
  assertions, so tests were moved to the documented parse/validate boundary;
- no-common-codec returned the specific `PJMEDIA_SDPNEG_NOANSCODEC` status;
- `RTP/AVP` and `RTP/SAVP` compare as compatible RTP/AVP-family transports in
  PJPROJECT 2.16, so the incompatible case uses `UDP`;
- the all-API probe signature and compiler-elided null dereference described
  above were corrected.

Final `git diff --check`, native/root CMake, forbidden-source, forbidden-symbol,
undefined-symbol, QEMU-process, and configuration-gate checks passed.
