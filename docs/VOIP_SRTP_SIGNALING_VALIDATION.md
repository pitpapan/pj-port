# VoIP SRTP Increment 6: SIP-Controlled SDES Media

Date: 2026-08-25

## Result

Mandatory SDES-SRTP call setup passes on `mps2/an385` under QEMU:

```text
SRTP SIGNALING RESULT: PASSED (RTP/SAVP; SIP-controlled media)
```

The opt-in `CONFIG_VOIP_PJ_SRTP_SIGNALING` gate routes the facade's PJSIP
INVITE SDP through PJMEDIA's SRTP transport hooks. Offers and answers are
upgraded
from the codec builder's base SDP to mandatory `RTP/SAVP` with exactly one
`AES_CM_128_HMAC_SHA1_80` SDES attribute. On successful negotiation, PJMEDIA
starts the primary SRTP transport from the active local and remote SDP. The
validation-side transport receives the crossed negotiated policies, allowing
generated G.711 audio to traverse the same encrypted packet boundary used by
the facade.

The existing-compatible public C++ facade is unchanged. No upstream
PJPROJECT protocol source was modified.

## Validation coverage

The QEMU test establishes an account and exercises outgoing and incoming calls
over SIP/TCP, then checks:

- the PJSIP offer/answer succeeds only with `RTP/SAVP` and strict SDES;
- call-owned keys and both SRTP adapters are active for the established call;
- PCMU RTP packets cross the SRTP/UDP boundary and decoded PCM reaches the
  bounded memory sink;
- local hold and resume re-INVITEs retain strict SDES and keep SRTP active;
- normal BYE teardown clears both key buffers and releases call resources;
- a second peer answer using plain `RTP/AVP` is rejected as a downgrade;
- downgrade failure actively terminates the INVITE rather than leaving a live
  dialog; and
- rejected-call transport teardown clears keys and leaves zero transactions
  and dialogs;
- an incoming `RTP/SAVP` offer is answered with strict SDES and carries
  encrypted PCMU media after facade acceptance; and
- a remote secure re-INVITE renegotiates without dropping SRTP, followed by
  remote BYE and complete key cleanup.

Build and run:

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/voip_integration \
  -d build_voip_srtp_call -- -DEXTRA_CONF_FILE=srtp_call.conf
west build -d build_voip_srtp_call -t run
```

Final footprint:

```text
FLASH: 363940 B / 4 MB
RAM:   2479600 B / 4 MB
```

Assertions were enabled. Logging was reduced during live signaling validation,
and no SDES key or `inline:` value was emitted.

## Current boundary and remaining work

SRTP is now complete for validated outgoing, incoming, hold/resume, and remote
re-INVITE paths through the C++ facade. It is **not production-ready security**
yet because SDES carries the media keys inside the still-plaintext SIP/TCP
session.

Remaining work:

1. port SIP TLS using the platform TLS implementation, with trusted-chain,
   hostname, validity-time, and failure-path validation;
2. make TLS mandatory whenever `CONFIG_VOIP_PJ_SRTP_SIGNALING` is selected;
3. extend and validate mandatory SDES across offerless INVITE, secure downgrade
   rejection on incoming and re-INVITE paths, CANCEL, and transport loss;
4. run a complete SIP-TLS-controlled SRTP PCMU/PCMA lifecycle and resource
   qualification; and
5. qualify hardware entropy, credential storage, key zeroization, and network
   behavior on MIMXRT1060.
