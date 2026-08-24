# VoIP Integration Phase 4 TCP Registration Validation

Date: 2026-08-24

## Result

Phase 4 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 4 RESULT: PASSED (3 registration lifecycles)
```

The C++ facade now performs real PJSIP registration and unregistration. A
transaction layer and `pjsip_regc` send REGISTER over TCP, process a Digest
challenge, refresh automatically, and unregister with expiry zero. Network
registration is independently selectable with
`CONFIG_VOIP_PJ_REGISTRATION_NETWORK`; Phase 3 retains its create-without-send
boundary when that option is disabled.

## Production behavior

`RegisterAccount()` and `UnregisterAccount()` submit synchronous bounded
commands to the shared PJPROJECT event thread. Completion means the request was
accepted by PJSIP; final results arrive asynchronously through the existing
observer facade.

The registration callback maps:

- successful registration and refresh to `registered` with SIP 200;
- successful unregister to `disabled` with SIP 200;
- 401/403 credential failure to `failed` and `authentication_failure`; and
- timeout or transport loss to `connection_lost` and `transport_failure`.

Transport failures schedule at most three automatic retries with bounded
exponential delays of 250, 500, and 1000 ms. Successful registration resets the
retry budget. Account replacement and shutdown cancel a pending retry before
destroying the registration client.

## Deterministic local registrar

The Phase 4 application installs an application-local PJSIP registrar module on
the backend's loopback TCP listener. It uses `pjsip_auth_srv` with a fixed realm,
nonce, opaque value, account, and password, and records the transport type of
every REGISTER. No SIP UDP transport is built or used.

Each of three lifecycles proves:

- fragmented OPTIONS input is not dispatched until the complete TCP message
  has arrived;
- two OPTIONS messages coalesced in one TCP send are dispatched separately;
- `REGISTER -> 401 -> authenticated REGISTER -> 200`;
- automatic refresh before expiry;
- expiry-zero unregister;
- wrong credentials produce SIP 403 and an authentication failure;
- SIP 408 produces connection loss and bounded automatic recovery;
- peer close before authentication produces connection loss and recovery;
- peer close after authentication produces connection loss and recovery;
- every parsed REGISTER arrived on `PJSIP_TRANSPORT_TCP`; and
- final endpoint teardown has zero timer entries.

Phase 3 already covers destruction while disabled, registering, registered, and
unregistering through controlled callbacks. Phase 4 additionally destroys live
network registration clients after success and after the failure/recovery
sequence.

## Build and runtime

Authoritative pristine build:

```sh
source .venv/bin/activate
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/voip_integration \
  -d build-voip-integration-phase4 -- \
  -DEXTRA_CONF_FILE=phase4_registration.conf
```

Final image footprint:

```text
FLASH: 201904 B / 4 MB
RAM:   560488 B / 4 MB
```

Bounded run:

```sh
timeout --signal=TERM --kill-after=3s 40s \
west build -d build-voip-integration-phase4 -t run
```

All three lifecycles completed in about 20 simulated seconds. The timeout exits
with 124 only after the pass marker because QEMU idles afterward.

## Configuration boundary

The validated profile includes:

```text
VOIP_PJ_REGISTRATION_NETWORK=y
PJSIP_REGC=y
PJSIP_TCP_TRANSPORT=y
PJSIP_UDP_TRANSPORT=n
NET_TCP=y
NET_UDP=n
PJSIP_INVITE=n
PJMEDIA_G711=n
PJMEDIA_RTP_RTCP=n
PJMEDIA_UDP_TRANSPORT=n
PJMEDIA_STREAM=n
PJMEDIA_AUDIODEV=n
```

No upstream PJPROJECT protocol source was modified.

## Regressions

Phase 3 passed all three account-only lifecycles with network registration
disabled:

```text
FLASH: 169332 B / 4 MB
RAM:   560232 B / 4 MB
VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)
```

Phase 2 was rebuilt pristine and passed all seven partial initialization cases
and five complete shared-runtime lifecycles:

```text
FLASH: 166116 B / 4 MB
RAM:   560232 B / 4 MB
VOIP INTEGRATION PHASE 2 RESULT: PASSED (5 runtime lifecycles)
```

## Remaining limitations

- The deterministic QEMU registrar is loopback-only; interoperability with the
  product registrar remains a hardware/network integration activity.
- DNS, TLS, redirects, and SIP UDP are not enabled by this profile.
- INVITE/dialog and call control begin in Phase 5.
- RTP over UDP, G.711, generated PCM, and the memory sink remain later phases.
