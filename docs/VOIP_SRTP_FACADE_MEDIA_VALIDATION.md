# VoIP SRTP Increment 5: Facade Media Transport

Date: 2026-08-25

## Result

The facade-owned headless media path passes on `mps2/an385` under QEMU:

```text
SRTP FACADE MEDIA RESULT: PASSED (PCMU/PCMA; encrypted UDP)
```

`CONFIG_VOIP_PJ_SRTP_MEDIA` wraps both facade-owned RTP/RTCP UDP transports in
PJMEDIA's mandatory SRTP adapter. Each prepared media session installs the two
distinct call-owned CSPRNG keys in opposite transmit/receive directions using
the single supported `AES_CM_128_HMAC_SHA1_80` suite. PJMEDIA streams remain
responsible for G.711, RTP/RTCP, and jitter buffering; the adapter protects and
authenticates packets before the UDP member transport sends or delivers them.

The public C++ facade is unchanged. No upstream PJPROJECT protocol source was
modified.

## Validation coverage

Three complete runtime lifecycles each exercise:

- PCMU frame generation, SRTP/UDP exchange, decode, and memory-sink receipt;
- normal SRTP transport stop, owned UDP close, and key zeroization;
- PCMA frame generation through a new pair of SRTP contexts and keys;
- injected destruction of one active SRTP/UDP transport;
- cleanup of the surviving stream and transport plus key zeroization; and
- complete PJLIB/PJSIP/PJMEDIA runtime teardown.

The prior SRTP transport validation separately proves that unauthenticated
plaintext injected through the UDP member is rejected by the receiving SRTP
adapter.

Build and run:

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/voip_integration \
  -d build_voip_srtp_media -- -DEXTRA_CONF_FILE=srtp_keys.conf
west build -d build_voip_srtp_media -t run
```

Final footprint:

```text
FLASH: 280864 B / 4 MB
RAM:   1151064 B / 4 MB
```

Assertions were enabled. The SRTP compile-time log ceiling remains level 4,
and no key bytes or SDP `inline:` values appeared in output.

## Port configuration defect corrected

The integrated link demonstrated that the Zephyr PJMEDIA profile had the
AES-CM-128 and AES-GCM-256 capability macros reversed. That made the upstream
suite table reference an unbuilt AES-GCM symbol. The port's `config_site.h`
now advertises AES-CM-128 and disables AES-GCM-256, matching the selected
libsrtp source closure and the documented security profile. This correction
is confined to port configuration.

## Remaining work

This increment secures the facade's generated-audio UDP path, but live PJSIP
calls still advertise `RTP/AVP` and do not exchange the SRTP keys. Remaining:

1. extend the validated outgoing, incoming, hold/resume, and remote re-INVITE
   SDES paths across offerless and remaining failure flows;
2. port SIP TLS with certificate-chain and hostname verification;
3. validate one complete SIP-TLS-controlled, SRTP-protected PCMU/PCMA call;
   and
4. qualify production entropy, credentials, zeroization, and resource bounds
   on MIMXRT1060.
