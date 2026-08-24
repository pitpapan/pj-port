# Phase 12: Robustness and Resource Validation

Use `phase12_robustness.conf` to run the supported one-call/two-stream profile
with longer active media and the complete signaling failure matrix.

The Phase 12-specific media run increases each active direction from 36 to 120
frames and rejects scheduling lateness greater than one 20 ms packet interval.
It also requires explicit `pjmedia_transport_media_stop()` before UDP transport
closure so no receive operation survives into the next signaling scenario.

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase12 -- -DEXTRA_CONF_FILE=phase12_robustness.conf
timeout --signal=TERM --kill-after=3s 30s \
  west build -d build-pjmedia-phase12 -t run
```

Expected marker:

```text
PHASE 12 ROBUSTNESS PROFILE: PASSED (extended lifecycle)
```

See `../PJMEDIA_PHASE12_VALIDATION.md` for measurements and exclusions.
