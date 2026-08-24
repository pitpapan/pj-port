# VoIP SRTP Increment 1: Cryptographic Primitives

Date: 2026-08-24

## Result

The first SRTP increment passes on `mps2/an385` under QEMU:

```text
SRTP PRIMITIVE RESULT: PASSED (AES_CM_128_HMAC_SHA1_80)
```

The PJPROJECT Zephyr module now has an opt-in `CONFIG_PJMEDIA_SRTP` boundary
that builds PJPROJECT's bundled libsrtp 2.1.0 implementation. The tested
profile is AES-128 counter mode with HMAC-SHA1-80, matching the initial G.711
media scope. No external TLS library is required for these primitives.

## Validated behavior

The deterministic target test creates independent sender and receiver SRTP
contexts and verifies:

- RTP protection adds the expected 80-bit authentication tag and encrypts the
  audio payload;
- the receiver authenticates, decrypts, and exactly recovers the RTP packet;
- a replayed protected RTP packet is rejected;
- a modified authentication tag is rejected;
- SRTCP protection and unprotection round-trip successfully; and
- contexts and global libsrtp state shut down cleanly, followed by explicit
  test-key zeroing.

The key is generated test data compiled into the validation application. It is
not a product key and no credential is printed.

## Build and execution

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build_srtp_primitives -- -DEXTRA_CONF_FILE=srtp_primitives.conf
west build -d build_srtp_primitives -t run
```

Final footprint:

```text
FLASH: 128328 B / 4 MB
RAM:   320240 B / 4 MB
```

Assertions were enabled. The build uses Zephyr's test random generator only
because it is inherited by this QEMU application; this deterministic primitive
test does not claim production entropy quality.

## Security boundary and remaining work

This increment proves the cryptographic packet engine only. Calls are **not yet
SRTP-secured**. The remaining increments are:

1. build and adapt PJMEDIA's SRTP transport over the existing UDP media
   transport (completed and validated in increment 2);
2. add SDP offer/answer for `RTP/SAVP` and `a=crypto`, with strict suite and
   key-length validation;
3. generate per-call master keys from a production CSPRNG, keep them out of
   logs, zero transient key material, and define the hardware key-storage
   boundary;
4. connect SRTP lifecycle and failures to the one-call C++ facade;
5. validate protected PCMU/PCMA media, tamper/replay rejection, teardown,
   failure paths, soak behavior, and resource use in QEMU; and
6. qualify entropy and key handling on MIMXRT1060 hardware.

SDES key exchange exposes SRTP key material inside SIP/SDP. Therefore SDES
must be paired with authenticated SIP TLS in production. TLS is a separate
signaling milestone and is not supplied by this increment. An alternative such
as DTLS-SRTP would replace the SDP key exchange work and has a larger porting
scope; it has not been selected here.

No upstream PJPROJECT protocol source was modified.
