# VoIP SRTP Increment 2: PJMEDIA Transport over UDP

Date: 2026-08-24

## Result

PJMEDIA's SRTP adapter transport passes on `mps2/an385` under QEMU:

```text
SRTP TRANSPORT RESULT: PASSED (UDP; AES_CM_128_HMAC_SHA1_80)
```

The opt-in `CONFIG_PJMEDIA_SRTP_TRANSPORT` gate now builds the upstream
PJMEDIA SRTP transport over the already-ported UDP media transport. It uses
the bundled libsrtp packet engine validated in SRTP increment 1. No upstream
PJPROJECT protocol source was modified.

## Validated behavior

The target test creates two IPv4 UDP transports, wraps each in a PJMEDIA SRTP
adapter, and configures distinct transmit and receive master keys. It verifies:

- authenticated and encrypted RTP travels from adapter A to adapter B and is
  delivered to the application only after exact plaintext recovery;
- SRTCP travels in the reverse direction and is recovered exactly;
- plaintext RTP injected through A's underlying UDP member, from the expected
  address and while the receive queue is actively polled, is rejected by B's
  SRTP adapter;
- the supported suite is limited to `AES_CM_128_HMAC_SHA1_80`; and
- detach, SRTP context destruction, owned UDP transport destruction, endpoint
  shutdown, and pool release complete cleanly.

The validation uses deterministic compiled test keys and explicitly zeroes
them after transport destruction.

## Key-log protection

PJMEDIA's upstream SRTP implementation contains level-5 diagnostic statements
that include base64 master keys. The Zephyr SRTP transport profile therefore
sets `PJ_LOG_MAX_LEVEL=4` at compile time. The accepted QEMU output contains no
key material. Product logging must preserve this ceiling or apply an equally
strong redaction policy.

## Build and execution

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build_srtp_transport -- -DEXTRA_CONF_FILE=srtp_transport.conf
west build -d build_srtp_transport -t run
```

Final footprint:

```text
FLASH: 182836 B / 4 MB
RAM:   631800 B / 4 MB
```

Assertions were enabled. QEMU's test random generator is not accepted as a
production entropy source.

## Security boundary and next increment

This increment validates explicit-key SRTP transport operation. Integrated
VoIP calls are not yet secured because SDP offer/answer and per-call key
generation are not connected.

The next increment was strict SDES negotiation using `RTP/SAVP` and
`a=crypto:... AES_CM_128_HMAC_SHA1_80 inline:...`; it is completed in
`VOIP_SRTP_SDES_VALIDATION.md`, including malformed input, wrong suite, wrong
key length, downgrade, key redaction, and teardown tests.
Because SDES carries its master key in SIP/SDP, the resulting mode is suitable
for production only when SIP TLS with certificate verification is active.
