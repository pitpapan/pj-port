### Completed

  - Established and validated the PJLIB Zephyr port, including networking and ioqueue stages.
  - Ported and validated PJSIP through robustness testing:
      - SIP over IPv4 TCP/UDP
      - Registration with Digest authentication
      - Transactions, shutdown, malformed packets, resource limits, and soak testing

  - Ported PJMEDIA through the headless media milestone:
      - SDP parsing and negotiation
      - INVITE call control
      - PCMU/PCMA G.711
      - RTP/RTCP and jitter buffer
      - UDP media transport
      - SIP-controlled media
      - Hold/resume and failure recovery

  - Completed VOIP integration Phases 1–9:
      - Stable C++ facade with no PJPROJECT types exposed
      - Shared PJPROJECT runtime and event thread
      - One-account/one-call bounded architecture
      - TCP SIP signaling and UDP RTP/RTCP media
      - Registration, incoming/outgoing calls, hold/resume, teardown, and recovery
      - Ten independent QEMU boots passed the complete lifecycle and active-media robustness matrix

  - Added SRTP support in three validated increments:
      - AES-CM-128/HMAC-SHA1-80 cryptographic primitives
      - PJMEDIA SRTP transport over UDP
      - Strict SDES negotiation using RTP/SAVP and a=crypto
      - Downgrade, malformed key, unsupported suite, replay, tamper, teardown, and key-log protection checks

  The main recent commits were:

  - e1cda1d9f — SRTP primitives
  - e80b9b8b7 — SRTP transport
  - d8a4ad887 — strict SRTP SDES negotiation

  ### Current status

  The QEMU VOIP integration target is complete, but SRTP is currently validated only in the PJMEDIA test application. It is not yet connected to the production C++ VOIP facade.

  The existing Phase 12 PJMEDIA documentation also notes that extended lifecycle testing passes, while some fault-injection and CPU/resource gates remain open.

  ### Future work

  - Add SIP TLS with certificate and hostname verification. This is required before SDES can be considered safe because SDES exposes SRTP keys in SIP/SDP.
  - Add a production CSPRNG-backed per-call key provider and define key zeroization/storage rules.
  - Integrate SRTP ownership and mandatory SDES negotiation into PjVoipBackend.
  - Validate a complete SIP-TLS-controlled, SRTP-protected PCMU/PCMA call, including tamper, replay, failure recovery, and soak tests.
  - Start MIMXRT1060 hardware integration:
      - ADC/eDMA capture
      - SPI DAC playback
      - CPU, latency, memory, DMA, overrun, and underrun qualification

  - Perform interoperability testing against the real registrar/proxy and peer.
  - Finalize production logging, credentials, entropy, memory limits, and unsupported-feature documentation.

  Repository note: the local dev branch is clean but one commit behind origin/dev. The pending commit changes SRTP transport configuration, enables MBEDTLS SSL sockets/AES-GCM-256, and reduces test heap/stack settings; it should be
  reviewed and validated before being treated as completed work.

  Useful references: docs/VOIP_INTEGRATION_PHASE9_VALIDATION.md, docs/VOIP_SRTP_SDES_VALIDATION.md, and docs/VOIP_PJPROJECT_INTEGRATION_PLAN.md.
