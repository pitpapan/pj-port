# Phase 7: PJMEDIA Endpoint and Direct G.711

## Objective

Create a PJMEDIA endpoint on the existing PJSIP ioqueue with zero media
workers, directly register G.711, and prove PCMU/PCMA conversion without RTP,
sockets, streams, or audio devices.

## Files introduced by this phase

- `applications/pjmedia_minimal/phase7_g711.conf`: runtime profile;
- `applications/pjmedia_minimal/phase7_link_probe.conf`: public API closure;
- `applications/pjmedia_minimal/phase7_disabled.conf`: default-off proof;
- `applications/pjmedia_minimal/src/phase7_g711.c`: lifecycle and vectors;
- `applications/pjmedia_minimal/src/phase7_link_probe.c`: retained API probe.

The Zephyr module CMake file conditionally adds the existing endpoint and
G.711 source groups. Do not change the root/native PJPROJECT CMake file.

## Build and run

From the workspace root:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase7 -- -DEXTRA_CONF_FILE=phase7_g711.conf
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase7 -t run
```

Required marker:

```text
PHASE 7 RESULT: PASSED (3 endpoint/G.711 lifecycles; PCMU+PCMA)
```

The timeout status is expected after the marker because QEMU idles when the
test finishes.

Then repeat a pristine build for `phase7_link_probe.conf` in
`build-pjmedia-phase7-link-probe`, and for `phase7_disabled.conf` in
`build-pjmedia-phase7-disabled`. The link probe must emit its pass marker. The
disabled archive must contain only the earlier SDP representation objects.

## What to verify

- exactly PCMU/PT 0 and PCMA/PT 8 are enumerated;
- fixed encode/decode vectors and malformed/short inputs pass;
- PLC and VAD behave correctly both enabled and disabled;
- G.711 deinitializes before endpoint destruction;
- the final caching pool is empty;
- archive and build-source audits find no RTP/RTCP, jitter buffer, media UDP,
  stream, audio-device, other codec, PJNATH, or PJSUA source;
- Phase 6 still builds and runs unchanged.

Do not add RTP/RTCP or jitter-buffer sources here; those belong to Phase 8.
