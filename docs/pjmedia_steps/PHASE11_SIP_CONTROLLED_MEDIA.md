# Phase 11: SIP-Controlled Headless Media

Phase 10 is intentionally skipped; its stream setup is folded into this phase.

Use `phase11_call.conf` to validate the complete path from INVITE/SDP through
bidirectional PCMU RTP and deterministic teardown. The key ordering is:

1. Allocate both RTP transports before creating SDP.
2. Complete offer/answer and wait for the confirmed dialog state.
3. Start each PJMEDIA transport with the active local and remote SDP.
4. Create and start both streams.
5. Exercise generated PCM, RTP/RTCP statistics, telephone events, and
   pause/resume.
6. End the dialog and destroy streams, transports, event manager, endpoint,
   and pools in dependency order.

The PJMEDIA endpoint must have a threadless event manager before stream
creation. `pjmedia_stream_create()` subscribes its RTCP session to that manager.

Build and run:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase11 -- -DEXTRA_CONF_FILE=phase11_call.conf
timeout --signal=TERM --kill-after=3s 40s \
  west build -d build-pjmedia-phase11 -t run
```

Completion marker:

```text
PHASE 11 RESULT: PASSED (integrated SIP-controlled headless G.711 media)
```

See `../PJMEDIA_PHASE11_VALIDATION.md` for the evidence and scope boundary.
