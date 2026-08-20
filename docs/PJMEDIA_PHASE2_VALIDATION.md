# PJMEDIA Phase 2 SDP Validation

Date: 2026-08-20

## Result

Phase 2 passed under QEMU. The Zephyr PJMEDIA library contains exactly the SDP
error, representation/parser/printer, and comparison objects:

```text
errno.c.obj
sdp.c.obj
sdp_cmp.c.obj
```

There is no SDP negotiation, media endpoint, codec implementation, RTP/RTCP,
media transport, stream, audio-device, video, SRTP, or ICE object or symbol in
the image. Phase 3 was not started.

## Zephyr-only parser policy

PJPROJECT 2.16 normally logs and silently truncates SDP media, format, and
attribute arrays that exceed their compile-time limits. That behavior does not
meet this port plan's boundary-rejection requirement. The Phase 2 change adds
strict behavior guarded by Zephyr configuration macros:

```text
PJMEDIA_SDP_MAX_PARSE_LEN=4096
PJMEDIA_SDP_REQUIRE_CRLF=1
PJMEDIA_SDP_STRICT_LIMITS=1
```

With this profile, overlong input returns `PJ_ETOOBIG`, malformed CR/LF input
returns `PJMEDIA_SDP_EINSDP`, and media/format/attribute/bandwidth overflow
returns `PJ_ETOOMANY`. Native platforms do not define these macros, so their
existing PJPROJECT parser behavior is unchanged. The root PJPROJECT
`CMakeLists.txt` is also unchanged.

## Test coverage

Each of three complete PJLIB lifecycles performed:

- PJLIB and PJLIB-UTIL initialization;
- explicit PJMEDIA error-range registration and lookup;
- minimal valid session parsing and semantic validation;
- multi-record IPv4 SDP parsing;
- PCMU, PCMA, and telephone-event `rtpmap` decoding at 8 kHz;
- telephone-event `fmtp` decoding;
- SDP print, reparse, semantic validation, and equality comparison;
- deep cloning of sessions, media, connections, bandwidths, and attributes;
- equal-clone and deliberately changed-clone comparison;
- `sendrecv`, `sendonly`, `recvonly`, and `inactive` parsing;
- missing-origin and invalid-payload semantic rejection;
- malformed line-ending, truncated-origin, and overlong-input rejection;
- fifth-media-line, seventeenth-format, and thirty-seventh-attribute rejection;
- pool release, zero checked-out caching-pool entries, and zero tracked PJ
  blocks after teardown.

A valid SDP session cannot literally contain only one SDP record because the
`v=`, `o=`, `s=`, and `t=` records are mandatory. The minimal-valid test uses
only those four records; the full test covers session and media records.

## Commands and results

Pristine build command:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase2 -- -DEXTRA_CONF_FILE=phase2_sdp.conf
```

The first build stopped in the validation application because
`pjlib-util.h` alone does not declare PJLIB string and memory helpers. Adding
the public `pjlib.h` include resolved the compile boundary; no production
source was added. The continued build completed successfully.

Final build footprint:

```text
FLASH: 110304 B / 4 MB (2.63%)
RAM:   194024 B / 4 MB (4.63%)
```

Runtime command:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
timeout --signal=TERM --kill-after=3s 10s \
  west build -d build-pjmedia-phase2 -t run
```

The first runtime attempt expected generic `PJMEDIA_SDP_EINSDP` for a
truncated origin. The parser correctly returned the more specific
`PJMEDIA_SDP_EINORIGIN`; the expectation was corrected and the full run then
passed.

Final markers:

```text
[Phase 2] valid parse/print/reparse/deep-clone/compare: PASSED
[Phase 2] sendrecv/sendonly/recvonly/inactive: PASSED
[Phase 2] malformed/truncated/overlong/configured limits: PASSED
[Phase 2] lifecycle 1 pool teardown: PASSED
[Phase 2] lifecycle 2 pool teardown: PASSED
[Phase 2] lifecycle 3 pool teardown: PASSED
PHASE 2 RESULT: PASSED (3 complete SDP lifecycles)
```

The runtime command returned 124 only because the bounded runner terminated
the idle QEMU instance after all markers. No QEMU process remained.

## Resource measurements

| Resource | Phase 2 result |
| --- | ---: |
| Zephyr heap | 131,072 B configured |
| PJ pool blocks | 1 block / 32,768 B peak |
| PJ pool used size | 12,424 B peak |
| PJ blocks after each teardown | 0 |
| Checked-out pools after each teardown | 0 |
| Main stack | 24,576 B configured |
| Main stack used | at most 9,952 B |
| Main stack unused | 14,624 B |

## Object and configuration audit

Commands:

```sh
rg -o "/home/pitpapan/zephyrproject/pjproject/pjmedia/src/pjmedia/[A-Za-z0-9_./-]+\\.c" \
  build-pjmedia-phase2/build.ninja | sort -u

/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-ar \
  t build-pjmedia-phase2/modules/pjproject/libpjmedia.a

/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm \
  -g build-pjmedia-phase2/zephyr/zephyr.elf | \
  rg "pjmedia_(endpt|codec|rtp|rtcp|transport|stream|aud|snd|sdp_neg)"

git diff --exit-code -- pjproject/CMakeLists.txt
git diff --check
ps -eo pid=,comm= | rg 'qemu-system-arm'
```

The source and archive queries returned exactly the three expected SDP
objects. The forbidden-symbol and QEMU process queries returned no matches.
The native/root CMake and whitespace checks passed.

The final Kconfig state contains `CONFIG_PJMEDIA=y` and
`CONFIG_PJMEDIA_SDP=y`, while SDP negotiation and the endpoint branch are
unset.

## Disabled-feature regression

The application was also rebuilt from pristine configuration without the
Phase 2 overlay:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase2-disabled
```

The build completed all 293 steps successfully. Its footprint was 76,028 B
FLASH and 35,128 B RAM. `build.ninja` contained no PJMEDIA source or library
entry, and `modules/pjproject/libpjmedia.a` was absent. This confirms that the
new integration remains feature-gated when `CONFIG_PJMEDIA` is disabled. The
disposable disabled-feature build directory was removed after this audit.
