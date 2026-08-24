# PJMEDIA Phase 11 SIP-Controlled Headless Media Validation

Date: 2026-08-24

## Result

Phase 11 passes on `mps2/an385` under QEMU:

```text
PHASE 11 RESULT: PASSED (integrated SIP-controlled headless G.711 media)
```

Phase 10 was intentionally skipped. Its stream-creation work is validated here
as part of the complete SIP-controlled media lifecycle.

No file under the Zephyr source directory was inspected or modified.

## Implemented media lifecycle

The Phase 11 harness:

- allocates UAC and UAS RTP sockets before generating SDP;
- binds each negotiated stream to the peer port advertised by signaling;
- creates the required threadless PJMEDIA event manager;
- starts each UDP media transport from the active local and remote SDP;
- creates and starts PCMU streams only after both INVITE sessions are confirmed;
- exchanges generated 20 ms PCM frames bidirectionally;
- validates RTP transmit/receive statistics, jitter-buffer access, RFC 2833
  telephone events, encoder pause, and resume;
- destroys streams, transports, the event manager, endpoint, and pools in
  dependency order.

The runtime repeats the complete PJLIB/PJSIP/PJMEDIA lifecycle three times and
retains the inherited call-control coverage for local and remote BYE,
CANCEL/487, rejection, retransmission, timeout, offerless INVITE,
re-INVITE hold/incompatible SDP, registration, and abrupt dialog cleanup.

## Validation commands

Runtime profile:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase11 -- -DEXTRA_CONF_FILE=phase11_call.conf

PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=3s 40s \
  west build -d build-pjmedia-phase11 -t run
```

Final runtime image:

```text
FLASH: 309556 B / 4 MB
RAM:   1155944 B / 4 MB
```

Representative media evidence from every lifecycle:

```text
[Phase 11] bidirectional PCM: UAC frames=35 hash=e447ee1e, UAS frames=35 hash=bf1c7ece; RTP tx/rx=34/36,36/34; JB=0: PASSED
[Phase 11] 20 ms cadence, telephone-event, pause, and resume: PASSED
```

Link-closure profile:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase11-link-probe \
  -- -DEXTRA_CONF_FILE=phase11_link_probe.conf
```

```text
PHASE 11 LINK PROBE: PASSED (integrated G.711 stream closure retained)
```

The link-probe image uses 108348 B flash and 594776 B RAM.

## Scope boundary

This phase proves headless SIP-controlled G.711 media over IPv4 UDP loopback.
It does not claim an audio device, hardware audio, IPv6, ICE, SRTP, video, or
production network interoperability.
