# PJMEDIA Phase 1 Validation

Date: 2026-08-20

## Result

Phase 1 passed. The Zephyr module now has an off-by-default PJMEDIA feature
graph, explicit but inactive candidate source families, a Zephyr-only media
configuration profile, and an independent `applications/pjmedia_minimal`
compile-boundary harness. No PJMEDIA implementation source is compiled or
linked, and no runtime media behavior is claimed.

The native PJPROJECT root `CMakeLists.txt` is unchanged. No Zephyr source was
inspected or modified.

## Implemented boundary

The production Kconfig graph contains:

```text
PJPROJECT -> PJLIB -> PJLIB_UTIL -> PJMEDIA
PJMEDIA -> SDP -> SDP_NEG
PJMEDIA + SDP -> ENDPOINT -> G711
ENDPOINT -> RTP_RTCP -> UDP_TRANSPORT
ENDPOINT + G711 + RTP_RTCP + UDP_TRANSPORT -> STREAM
ENDPOINT -> AUDIODEV -> NULL or ZEPHYR backend
PJSIP + SDP_NEG -> PJSIP_INVITE
```

Every downstream symbol has an explicit `depends on` relationship. The
Zephyr audio backend also depends on `CONFIG_AUDIO` and does not select a
board or driver.

`pjproject/zephyr/CMakeLists.txt` names candidate families for SDP,
negotiation helpers, endpoint/runtime, RTP/RTCP, UDP transport, stream, G.711,
audio-device core, null audio, and the future Zephyr backend. Phase 1 does not
pass any of these lists to `zephyr_library_sources()`. Unsupported video,
SRTP, ICE, resampling, AEC, legacy sound, and other-codec lists remain empty.

The Zephyr profile explicitly selects audio-only behavior, G.711 only, no
conversion table, SDP bounds of 4 media lines and 16 formats, and four custom
format-negotiation callbacks. It disables video, SRTP/DTLS, RTCP XR,
resampling, Speex/WebRTC AEC, FFmpeg, libyuv, legacy sound, other codecs, and
all host audio backends.

## Commands and results

Static review:

```sh
git diff --check
git diff -- pjproject/Kconfig pjproject/zephyr/CMakeLists.txt \
  pjproject/pjlib/include/pj/config_site.h applications/pjmedia_minimal
```

`git diff --check` passed.

Disabled/default boundary:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase1-disabled
```

The first attempt failed while compiling the application because the new
`prj.conf` enabled PJLIB/POSIX but omitted the validated network/socket
platform settings required by `pjlib.h`; `sa_family_t` was unavailable. The
PJMEDIA application configuration was aligned with the existing validated
PJLIB network baseline, and the same pristine command then passed all 293
build steps:

```text
FLASH: 76028 B / 4 MB
RAM:   35128 B / 4 MB
```

Before deleting this disposable build directory to recover disk space, these
audits were executed:

```sh
rg -n "pjmedia/src|PJMEDIA" build-pjmedia-phase1-disabled/build.ninja
rg -n "CONFIG_PJMEDIA(=|_)" \
  build-pjmedia-phase1-disabled/zephyr/.config
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-size \
  build-pjmedia-phase1-disabled/zephyr/zephyr.elf
```

Both `rg` commands produced no matches. The size command reported text/data/bss
as 74,568/1,460/33,682 bytes.

Enabled compile boundary:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase1 -- -DEXTRA_CONF_FILE=phase1_boundary.conf
```

The first enabled attempt compiled `phase1_boundary.c` successfully, then
failed later in a Zephyr networking object because the filesystem containing
`/tmp` had only 372 KiB available. The generated partial Phase 1 directory and
the already-audited disabled directory were removed, yielding 54 MiB free.
The identical pristine enabled command then passed all 299 steps:

```text
FLASH: 66440 B / 4 MB
RAM:   29800 B / 4 MB
```

After the candidate lists were made explicit, this incremental reconfigure
confirmed they remained inactive and the image stayed unchanged:

```sh
west build -d build-pjmedia-phase1
```

QEMU compile-boundary marker:

```sh
timeout --signal=TERM --kill-after=3s 15s \
  west build -d build-pjmedia-phase1 -t run
```

Output:

```text
PJMEDIA minimal Zephyr application
PJMEDIA Phase 1 configuration/header boundary: PASSED
No PJMEDIA production object is linked in Phase 1
```

The wrapper returned 124 because it stopped the idle QEMU instance after the
marker. No QEMU process remained.

Final object, symbol, and native-build audits:

```sh
rg -n "pjmedia/src" build-pjmedia-phase1/build.ninja
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm \
  -g build-pjmedia-phase1/zephyr/zephyr.elf | rg "pjmedia_"
git diff --exit-code -- pjproject/CMakeLists.txt
git diff --check
ps -eo pid=,comm= | rg 'qemu-system-arm'
```

The PJMEDIA implementation-object query and global-symbol query returned no
matches. The root CMake diff and whitespace checks passed. The process query
returned no match.

The only PJMEDIA-related application object in `build.ninja` is
`src/phase1_boundary.c.obj`; the PJMEDIA public include directory is present
only in the enabled build.

The existing Phase 11 graph was also regenerated with PJMEDIA disabled:

```sh
west build -d build-pjsip-phase11 -t build_info
```

CMake/Kconfig regeneration completed successfully and selected PJLIB,
PJLIB-UTIL, and PJSIP without selecting PJMEDIA. The command then returned 1
because this build exposes no Ninja target named `build_info`; no compilation
was requested or needed for this graph-only regression check. The full Phase
11 build and runtime regression remains the Phase 0 evidence.

## Scope boundary

Phase 2 was not started. In particular, `errno.c`, `sdp.c`, and `sdp_cmp.c`
are named as an inactive candidate group but are not compiled or linked.
There is no SDP parser runtime claim, endpoint, RTP/RTCP, stream, codec
implementation, or audio-device implementation in the Phase 1 image.
