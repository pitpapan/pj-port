# VoIP SRTP Increment 4: Per-Call Key Lifecycle

Date: 2026-08-25

## Result

The application-owned SRTP key lifecycle passes on `mps2/an385` under QEMU:

```text
SRTP FACADE MEDIA RESULT: PASSED (PCMU/PCMA; encrypted UDP)
```

The opt-in `CONFIG_VOIP_PJ_SRTP_KEY_LIFECYCLE` gate adds two fixed-size,
non-copyable 30-byte master-key/salt owners to the one-call headless-media
backend. Every call preparation obtains fresh bytes through Zephyr's
`sys_csrand_get()` interface. Generation failure aborts preparation, equal
local and peer keys are rejected, and both buffers are erased through volatile
writes during normal teardown, injected transport failure, initialization
failure cleanup, and destruction.

The existing-compatible `VoipFacade.hpp` contract is unchanged. Validation
state is exposed only by the concrete PJPROJECT backend. No upstream
PJPROJECT protocol source was changed.

## Target validation

The test performs three complete runtime lifecycles. Each lifecycle checks:

- key buffers begin inactive and cleared;
- PCMU preparation activates two distinct call-owned keys;
- normal media teardown clears both buffers;
- PCMA preparation creates another active key pair;
- injected UDP transport failure followed by teardown clears both buffers; and
- PJLIB, PJSIP, PJMEDIA, pool, transport, stream, and thread resources shut
  down cleanly.

Build and run:

```sh
source .venv/bin/activate
export CCACHE_DIR=/home/pitpapan/zephyrproject/.ccache
export CCACHE_TEMPDIR=/tmp/voip-ccache
west build -p always -b mps2/an385 applications/voip_integration \
  -d build_voip_srtp_keys -- -DEXTRA_CONF_FILE=srtp_keys.conf
west build -d build_voip_srtp_keys -t run
```

Final footprint:

```text
FLASH: 280864 B / 4 MB
RAM:   1151064 B / 4 MB
```

Assertions were enabled. No key bytes or SDP `inline:` values were logged.

## Security boundary

QEMU uses `CONFIG_TEST_RANDOM_GENERATOR`, which Zephyr explicitly identifies
as unsuitable for production cryptography. This validates the error handling,
ownership, uniqueness check, and zeroization lifecycle only. MIMXRT1060 must
provide and qualify a production entropy source before this gate can be used
in a product build.

The same validation target now also selects the later SRTP-media gate, so it
proves that the owned keys are installed into the facade's PJMEDIA SRTP
transports. SIP/SDES signaling is validated separately by
`VOIP_SRTP_SIGNALING_VALIDATION.md`.

## Remaining work

1. port SIP TLS with certificate-chain and hostname verification so SDES keys
   are not exposed on the signaling path;
2. extend strict SDES/SRTP validation across incoming, offerless, hold/resume,
   re-INVITE, and failure paths;
3. validate a complete SIP-TLS-controlled, SRTP-protected PCMU/PCMA call; and
4. qualify entropy, credentials, zeroization, and resource bounds on
   MIMXRT1060.
