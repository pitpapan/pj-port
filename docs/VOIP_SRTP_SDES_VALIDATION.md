# VoIP SRTP Increment 3: Strict SDES Negotiation

Date: 2026-08-24

## Result

Strict PJMEDIA SDES offer/answer passes on `mps2/an385` under QEMU:

```text
SRTP SDES RESULT: PASSED (mandatory RTP/SAVP; strict suite)
```

The new opt-in `CONFIG_PJMEDIA_SRTP_SDES` gate enables PJMEDIA's RFC 4568
keying implementation on top of the already validated SRTP transport. No
upstream PJPROJECT protocol source was modified.

## Accepted profile

The validation constrains negotiation to:

- `RTP/SAVP` media transport;
- one `a=crypto` line;
- tag `1`;
- `AES_CM_128_HMAC_SHA1_80` only;
- an `inline` 30-byte master key and salt encoded as exactly 40 base64
  characters; and
- distinct local keys for the offerer and answerer.

Both sides complete `media_create`, SDP encoding, and `media_start`, proving
that the resulting transmit and receive policies create active libsrtp
contexts. Both sides then stop and destroy their transports cleanly.

## Rejection coverage

The mandatory profile rejects:

- an `RTP/AVP` downgrade;
- `RTP/SAVP` without an `a=crypto` attribute;
- invalid base64 key material;
- decoded key material shorter than 30 bytes;
- an unsupported crypto-suite name; and
- a crypto tag with a forbidden leading zero.

These cases exercise PJMEDIA's existing SDES parser and negotiation logic; no
parallel SDP security parser was added to the application.

## Key handling boundary

The QEMU test supplies deterministic, distinct 30-byte keys so PJMEDIA's
non-cryptographic fallback generator is never used. Test keys are zeroed after
transport teardown. The SRTP build retains the compile-time
`PJ_LOG_MAX_LEVEL=4` ceiling because upstream level-5 diagnostics can print
base64 keys. Accepted runtime output contains neither `inline:` values nor key
material.

This increment does **not** provide a production CSPRNG or secure key storage.
The integrated facade must always provide a freshly generated per-call key and
must never permit PJMEDIA to auto-generate one through its fallback path.

## Build and execution

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build_srtp_sdes -- -DEXTRA_CONF_FILE=srtp_sdes.conf
west build -d build_srtp_sdes -t run
```

Final footprint:

```text
FLASH: 178352 B / 4 MB
RAM:   625480 B / 4 MB
```

Assertions were enabled. No key material appeared in the accepted QEMU log.

## Remaining production work

SDES places the SRTP master key inside SIP/SDP. Consequently, this feature is
not production-safe over the project's current plain SIP/TCP transport. Before
the facade can enable SDES-SRTP in production, the remaining work is:

1. port SIP TLS with certificate and hostname verification;
2. add a Zephyr CSPRNG-backed per-call key provider and hardware key-lifetime
   policy;
3. integrate mandatory SDES negotiation and SRTP transport ownership into the
   one-call C++ facade;
4. validate a complete SIP-TLS-controlled, SRTP-protected PCMU/PCMA call; and
5. qualify entropy, credentials, zeroization, and resources on MIMXRT1060.
