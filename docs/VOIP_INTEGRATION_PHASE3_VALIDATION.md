# VoIP Integration Phase 3 Account Validation

Date: 2026-08-24

## Result

Phase 3 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)
```

`PjVoipBackend` now owns one bounded account configuration and one PJSIP
registration client (`pjsip_regc`). It initializes the client with the account
identity, registrar, contact, expiry, and one Digest credential while retaining
the Phase 2 shared runtime and mandatory SIP-over-TCP factory.

This phase deliberately does not send REGISTER. `RegisterAccount()` and
`UnregisterAccount()` continue to return `invalid_state`; live TCP registration,
authentication challenges, refresh, and unregister are Phase 4 work.

## Account ownership and bounds

The backend copies account data into its private fixed-capacity storage before
submitting the synchronous command to the event thread. Limits are one account,
one registration client, 127 bytes per SIP URI, 63 bytes for the username, and
127 bytes for the password. Null fields and a zero expiry are rejected as
`invalid_argument`; oversized fields are rejected as `value_too_long`.

Replacing the account destroys the previous registration client on the event
thread before creating and initializing its replacement. Shutdown destroys the
registration client before the TCP factory and endpoint, then zeroes all URI,
username, and password buffers. The backend contains no credential logging.

When `CONFIG_PJSIP_REGC` is disabled, account configuration returns
`invalid_state` and no registration-client functions are referenced. This
preserves the smaller Phase 2 configuration boundary.

## Registration state delivery

The facade observer boundary now receives registration states dispatched on
the PJPROJECT event thread. Phase 3 uses controlled state injection rather than
network traffic to validate `disabled`, `registering`, `registered`, and
`unregistering`, including SIP status propagation. An observer can remove
itself from inside its callback; the following state change is not delivered.

The network registration callback remains inert until Phase 4 connects it to
the same observer path and translates PJSIP status.

## Build and runtime

Authoritative pristine build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase3 -- \
  -DEXTRA_CONF_FILE=phase3_account.conf
```

Image footprint:

```text
FLASH: 169244 B / 4 MB
RAM:   560232 B / 4 MB
```

Authoritative bounded run:

```sh
timeout --signal=TERM --kill-after=3s 10s \
west build -d build-voip-integration-phase3 -t run
```

All three lifecycles passed in less than one simulated second. The command
returned 124 only after the pass marker because QEMU idled afterward.

Each lifecycle validates rejected invalid input, registration-client creation,
four ordered observer callbacks, repeated account replacement, safe observer
self-removal, and complete teardown. Endpoint teardown reported zero timer-heap
entries in every lifecycle. Runtime output contained no account credentials.

## Configuration and source boundary

The Phase 3 overlay enables the registration client and TCP signaling while
keeping later protocol and media components disabled:

```text
PJSIP_REGC=y
PJSIP_TCP_TRANSPORT=y
PJSIP_UDP_TRANSPORT=n
PJSIP_INVITE=n
PJMEDIA_G711=n
PJMEDIA_RTP_RTCP=n
PJMEDIA_UDP_TRANSPORT=n
PJMEDIA_STREAM=n
PJMEDIA_AUDIODEV=n
```

No upstream PJPROJECT protocol source was modified for Phase 3.

## Phase 2 regression

The Phase 2 profile was rebuilt pristine with `PJSIP_REGC=n`. All seven
partial initialization cleanup cases and five shared-runtime lifecycles passed:

```text
FLASH: 166084 B / 4 MB
RAM:   560232 B / 4 MB
VOIP INTEGRATION PHASE 2 RESULT: PASSED (5 runtime lifecycles)
```

## Remaining limitations

- Phase 3 creates and configures `pjsip_regc` but performs no network request.
- Phase 4 must prove REGISTER and unregister over TCP, Digest challenge handling,
  refresh/expiry behavior, response mapping, and loss/recovery behavior.
- INVITE/dialog, RTP over UDP, G.711, generated PCM, and the memory sink remain
  disabled for their later phases.
- Hardware ADC/eDMA capture and SPI DAC playback remain deferred to the
  MIMXRT1060 hardware phase.
