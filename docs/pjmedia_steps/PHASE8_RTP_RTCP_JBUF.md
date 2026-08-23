# Phase 8: RTP, RTCP, and Jitter-Buffer Primitives

## Objective

Validate RTP packet state, RTCP reports/statistics, and jitter-buffer behavior
without creating sockets, transports, streams, or audio devices.

The production gate is `CONFIG_PJMEDIA_RTP_RTCP`. It selects exactly
`rtp.c`, `rtcp.c`, `rtcp_fb.c`, and `jbuf.c`. Feedback is included because
the RTCP compound parser directly calls its feedback parsers.

## Build and run

From `/home/pitpapan/zephyrproject`:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase8 -- -DEXTRA_CONF_FILE=phase8_packet.conf
timeout --signal=TERM --kill-after=5s 20s \
  west build -d build-pjmedia-phase8 -t run
```

Required marker:

```text
PHASE 8 RESULT: PASSED (3 socket-free RTP/RTCP/jitter lifecycles)
```

After the marker, timeout status 124 is expected because the sample idles.

Repeat pristine builds using `phase8_link_probe.conf` and
`phase8_disabled.conf` in their matching build directories. Require:

```text
PHASE 8 LINK PROBE: PASSED (RTP/RTCP/jitter public closure retained)
PHASE 8 DISABLED: PASSED (no RTP/RTCP/jitter objects)
```

## Audit checklist

- Enabled archive: eight earlier SDP/endpoint objects plus exactly `rtp.c`,
  `rtcp.c`, `rtcp_fb.c`, and `jbuf.c`.
- Disabled archive: only the eight earlier objects.
- Runtime and probe ELFs: no undefined symbols.
- Build graph: no transport, stream, audio-device, PJNATH, PJSUA, video,
  SRTP, ICE, or RTCP-XR implementation.
- Runtime: three lifecycles, final resource marker, clean pool teardown.
- Phase 7: rebuild and rerun unchanged.

Do not add `transport_loop.c` or `transport_udp.c`; those belong to Phase 9.
