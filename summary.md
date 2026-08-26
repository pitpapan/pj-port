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





• These are three separate implementation increments. Each should build and pass independently before connecting the next one.

  PJLIB SSL/mbedTLS
          ↓
  PJSIP TLS transport
          ↓
  Facade uses only TLS
          ↓
  SDES permitted

  ## 2. Port PJLIB SSL over mbedTLS

  ### 2.1 Correct the current configuration boundary

  At present, pjproject/pjlib/include/pj/config_site.h:9 unconditionally says:

  #define PJ_HAS_SSL_SOCK 1
  #define PJ_SSL_SOCK_IMP PJ_SSL_SOCK_IMP_MBEDTLS

  But the actual mbedTLS implementation is not compiled. Make this conditional:

  #if defined(CONFIG_PJLIB_SSL_MBEDTLS)
  #  define PJ_HAS_SSL_SOCK 1
  #  define PJ_SSL_SOCK_IMP PJ_SSL_SOCK_IMP_MBEDTLS
  #else
  #  define PJ_HAS_SSL_SOCK 0
  #  define PJ_SSL_SOCK_IMP PJ_SSL_SOCK_IMP_NONE
  #endif

  This prevents headers from claiming SSL support when the implementation is missing.

  ### 2.2 Add a Kconfig gate

  Add to pjproject/Kconfig:

  config PJLIB_SSL_MBEDTLS
      bool "PJLIB SSL socket using mbedTLS"
      depends on PJLIB
      depends on NET_SOCKETS && NET_TCP
      depends on MBEDTLS
      default n
      help
        Build PJLIB's asynchronous SSL socket using the Zephyr-provided
        mbedTLS implementation. A production configuration must provide
        cryptographic entropy and a trustworthy certificate time source.

  Do not enable it by default yet.

  The application TLS configuration will also need the public Zephyr mbedTLS options for:

  - TLS client
  - TLS server, if incoming SIP connections are supported
  - X.509 certificate parsing
  - CA-chain verification
  - SHA-256
  - RSA and/or ECDSA, based on the selected certificates
  - entropy and CTR-DRBG
  - PEM parsing if certificates are embedded as PEM
  - TLS 1.2 or newer

  Exact Zephyr symbols should be selected through documented Kconfig interfaces and confirmed through build output, without inspecting
  Zephyr implementation sources.

  ### 2.3 Add the mbedTLS source closure

  The Zephyr PJLIB source list currently includes only:

  pjlib/src/pj/ssl_sock_common.c

  Under CONFIG_PJLIB_SSL_MBEDTLS, add:

  pjlib/src/pj/ssl_sock_mbedtls.c

  to pjproject/zephyr/CMakeLists.txt:1:

  zephyr_library_sources_ifdef(CONFIG_PJLIB_SSL_MBEDTLS
    ${PJPROJECT_ROOT_DIR}/pjlib/src/pj/ssl_sock_mbedtls.c
  )

  The PJLIB target must receive the mbedTLS includes and libraries exported by Zephyr.

  Do not add AES-GCM libsrtp sources here. TLS mbedTLS and SRTP AES-GCM are separate consumers.

  ### 2.4 Expected compatibility points

  PJPROJECT’s backend uses:

  - mbedtls_ssl_context
  - mbedtls_ssl_config
  - mbedtls_entropy_context
  - mbedtls_ctr_drbg_context
  - mbedtls_x509_crt
  - mbedtls_pk_context
  - mbedtls_ssl_set_hostname()
  - asynchronous PJLIB socket callbacks

  Likely areas needing validation are:

  1. mbedTLS version

     The PJPROJECT backend must match the mbedTLS API version supplied by the selected Zephyr configuration.

  2. Entropy

     mbedtls_ctr_drbg_seed() must reach the production entropy source. QEMU may use the test random generator only for validation.

  3. Time

     X.509 validity checking must receive a meaningful time.

  4. Memory allocation

     mbedTLS allocation must work with the configured embedded heap, with bounded handshake memory.

  5. Nonblocking I/O

     WANT_READ and WANT_WRITE must integrate correctly with the PJLIB active-socket/ioqueue loop.

  6. PEM versus DER

     PEM costs additional code and RAM. QEMU can start with PEM buffers; hardware may use DER or another provisioned representation.

  Only modify ssl_sock_mbedtls.c if a test proves a real compatibility defect. Prefer configuration or a Zephyr-specific adapter
  first.

  ### 2.5 Certificate ownership

  Start with application-owned immutable buffers:

  struct TlsCredentials {
      const std::uint8_t *ca;
      std::size_t ca_size;

      const std::uint8_t *certificate;
      std::size_t certificate_size;

      const std::uint8_t *private_key;
      std::size_t private_key_size;

      const char *private_key_password;
  };

  For a client-only device:

  - CA is mandatory.
  - Device certificate/private key is optional unless the server requires mutual TLS.

  For a TLS listener:

  - local certificate and private key are mandatory.
  - CA is required only when authenticating client certificates.

  Ensure buffers remain valid for the entire TLS transport lifetime, or copy them into explicitly owned storage.

  Private-key storage must be wiped when released. PJPROJECT provides key-wiping APIs, but the application’s original credential
  buffer must also follow its own ownership policy.

  ### 2.6 PJLIB-only validation application

  Before adding PJSIP TLS, make a PJLIB SSL socket validation target.

  Test topology:

  PJLIB TLS client ── loopback TCP ── PJLIB TLS server

  Positive test:

  1. Initialize PJLIB, pool, ioqueue and timer heap.
  2. Load a test CA.
  3. Load the server certificate/private key.
  4. Create a TLS server socket.
  5. Bind it to 127.0.0.1 with an ephemeral port.
  6. Create a TLS client socket.
  7. Set the server name matching the certificate SAN.
  8. Connect.
  9. Complete the asynchronous handshake.
  10. Exchange data in both directions.
  11. Perform an orderly TLS shutdown.
  12. Destroy sockets and pools.
  13. Confirm zero live resources.

  Negative tests:

  - unknown CA
  - wrong DNS hostname
  - expired certificate
  - not-yet-valid certificate
  - malformed certificate
  - missing server certificate
  - connection closed during handshake
  - handshake timeout
  - allocation failure if fault injection is available

  Expected result:

  PJLIB MBEDTLS RESULT: PASSED

  Do not proceed to PJSIP until hostname and CA failures are correctly rejected.

  ———

  ## 3. Enable and validate PJSIP TLS

  ### 3.1 Add the PJSIP TLS gate

  Add:

  config PJSIP_TLS_TRANSPORT
      bool "PJSIP TLS transport"
      depends on PJSIP && PJLIB_SSL_MBEDTLS
      depends on NET_SOCKETS && NET_TCP
      default n
      help
        Build the PJSIP reliable TLS signaling transport over the validated
        PJLIB mbedTLS SSL socket.

  Change pjproject/pjlib/include/pj/config_site.h:120:

  #if defined(CONFIG_PJSIP_TLS_TRANSPORT)
  #  define PJSIP_HAS_TLS_TRANSPORT 1
  #else
  #  define PJSIP_HAS_TLS_TRANSPORT 0
  #endif

  ### 3.2 Compile the PJSIP TLS source

  Add under the PJSIP library in pjproject/zephyr/CMakeLists.txt:260:

  zephyr_library_sources_ifdef(CONFIG_PJSIP_TLS_TRANSPORT
    ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip/sip_transport_tls.c
  )

  The dependency chain should be:

  pjsip
    → pjlib-util
    → pjlib
    → mbedTLS

  No PJMEDIA dependency is required for a PJSIP TLS transport test.

  ### 3.3 Add backend-owned TLS state

  The PJSIP backend currently owns TCP state such as tcp_port. Add TLS-specific state:

  pjsip_tpfactory *tls_factory{};
  std::uint16_t tls_port{};
  bool tls_initialized{};
  bool tls_peer_verified{};
  pjsip_transport *verified_tls_transport{};

  Also retain or own:

  - CA buffer
  - local certificate
  - private key
  - expected registrar hostname
  - TLS handshake timeout
  - mutual-TLS policy

  Clear all transport and verification state during shutdown and disconnect.

  ### 3.4 Configure pjsip_tls_setting

  During backend initialization:

  pjsip_tls_setting tls;
  pjsip_tls_setting_default(&tls);

  tls.ca_buf.ptr =
      reinterpret_cast<char *>(const_cast<std::uint8_t *>(credentials.ca));
  tls.ca_buf.slen = credentials.ca_size;

  tls.cert_buf.ptr =
      reinterpret_cast<char *>(
          const_cast<std::uint8_t *>(credentials.certificate));
  tls.cert_buf.slen = credentials.certificate_size;

  tls.privkey_buf.ptr =
      reinterpret_cast<char *>(
          const_cast<std::uint8_t *>(credentials.private_key));
  tls.privkey_buf.slen = credentials.private_key_size;

  tls.verify_server = PJ_TRUE;
  tls.verify_client = mutual_tls ? PJ_TRUE : PJ_FALSE;
  tls.require_client_cert = mutual_tls ? PJ_TRUE : PJ_FALSE;

  tls.timeout.sec = 5;
  tls.timeout.msec = 0;

  Use a TLS version policy supported by both the PJPROJECT mbedTLS backend and Zephyr mbedTLS configuration. Do not enable TLS 1.0 or
  TLS 1.1.

  For hostname verification, use a DNS name:

  tls.server_name = pj_str(registrar_hostname);

  Do not use a raw IP address as SNI. If QEMU connects to loopback, the test certificate should contain an appropriate DNS SAN and the
  connection should retain that expected DNS identity even if routing resolves it to 127.0.0.1.

  ### 3.5 Start the TLS transport

  The current backend starts TCP around voip/src/PjVoipBackend.cpp:1093. Add a secure alternative:

  pj_sockaddr local;
  pj_bzero(&local, sizeof(local));
  local.addr.sa_family = pj_AF_INET();

  pj_status_t status = pjsip_tls_transport_start2(
      sip_endpoint,
      &tls,
      &local,
      nullptr,
      1,
      &tls_factory);

  For the one-call embedded profile, one asynchronous accept is a reasonable starting point.

  If only outbound SIP is required, consider the PJSIP option that avoids creating a listener. If incoming connections are required,
  start a listener and provide the local certificate/private key.

  Store the actual bound port for validation, similar to the current TCP port.

  ### 3.6 Use secure SIP URIs

  Change the secure configuration from:

  sip:alice@example.com
  sip:registrar.example.com:5060;transport=tcp

  to:

  sips:alice@example.com
  sips:registrar.example.com:5061;transport=tls

  Outgoing destinations should also use:

  sips:peer@example.com;transport=tls

  Contacts generated by the backend must become:

  <sips:alice@device.example.com:5061;transport=tls>

  Using only transport=tls with an ordinary sip: URI can be useful in some deployments, but sips: expresses the stronger requirement
  that the SIP route remain secure. For this project’s mandatory policy, prefer sips:.

  ### 3.7 Bind operations to the TLS factory

  Do not rely only on URI selection. Explicitly bind the registration and dialog to the TLS transport factory through a
  pjsip_tpselector.

  Conceptually:

  pjsip_tpselector selector{};
  selector.type = PJSIP_TPSELECTOR_LISTENER;
  selector.u.listener = tls_factory;

  Apply it to:

  - the registration client with pjsip_regc_set_transport()
  - outgoing dialogs with pjsip_dlg_set_transport()

  This prevents PJSIP from selecting the existing TCP listener because of address or URI ambiguity.

  ### 3.8 Verify the actual connection

  “Transport type is TLS” and “peer is authenticated” are different facts.

  Extend the existing transport-state callback to record:

  - TLS transport connected
  - certificate verification result
  - remote certificate identity
  - verification failure flags
  - disconnect reason

  Only set:

  tls_peer_verified = true;

  when:

  - the transport type is TLS;
  - the TLS handshake succeeded;
  - verify_server was enabled;
  - verification status contains no errors;
  - hostname verification succeeded.

  Bind that flag to the specific transport pointer:

  verified_tls_transport = transport;

  Clear both on disconnect.

  Do not use one global boolean that could survive transport replacement.

  ### 3.9 PJSIP-only TLS test

  Before combining TLS with calls, validate:

  PJSIP TLS client → PJSIP TLS listener

  Test:

  1. Start one PJSIP TLS listener.
  2. Send OPTIONS through TLS.
  3. Receive and parse it through PJSIP.
  4. Return 200 OK.
  5. Verify both request and response used TLS.
  6. Verify the client authenticated the server certificate.
  7. Close the connection.
  8. Verify zero transports, transactions and timers.

  Negative cases:

  - attempt the request over TCP while secure-only mode is active
  - wrong CA
  - wrong hostname
  - handshake timeout
  - connection loss before complete SIP message
  - connection loss after sending a request
  - malformed SIP delivered inside a valid TLS session
  - repeated connect/disconnect lifecycle

  Expected result:

  PJSIP TLS TRANSPORT RESULT: PASSED

  ———

  ## 4. Require TLS for SDES

  This requirement must exist at build time and runtime.

  ## 4.1 Build-time dependency

  The production form should be:

  config VOIP_PJ_SIP_TLS
      bool "VoIP PJSIP TLS signaling"
      depends on VOIP_PJ_BACKEND && PJSIP_TLS_TRANSPORT
      default n

  Then change the current SRTP signaling gate in voip/zephyr/Kconfig:72:

  config VOIP_PJ_SRTP_SIGNALING
      bool "VoIP mandatory SRTP/SDES call signaling"
      depends on VOIP_PJ_SRTP_MEDIA && VOIP_PJ_CALL_CONTROL
      depends on PJMEDIA_SRTP_SDES
      depends on VOIP_PJ_SIP_TLS

  The secure production configuration must therefore be impossible unless all of these are enabled:

  PJLIB_SSL_MBEDTLS=y
  PJSIP_TLS_TRANSPORT=y
  VOIP_PJ_SIP_TLS=y
  PJMEDIA_SRTP=y
  PJMEDIA_SRTP_TRANSPORT=y
  PJMEDIA_SRTP_SDES=y
  VOIP_PJ_SRTP_SIGNALING=y

  ### Preserving the current QEMU SRTP test

  The existing SRTP test intentionally uses plaintext loopback SIP/TCP. When making TLS mandatory, either:

  1. migrate that test immediately to TLS; or
  2. temporarily introduce an explicitly unsafe test-only option:

  config VOIP_PJ_ALLOW_INSECURE_SDES_TEST
      bool
      depends on TEST
      default n

  Production configurations must never select it.

  The better end state is to migrate srtp_call.conf completely to TLS and remove the exception.

  ## 4.2 Check outgoing calls before producing SDES

  Before calling:

  headless_media->EncodeSdesOffer(...)

  require:

  bool SecureSignalingReady() const noexcept {
      return tls_initialized &&
             tls_factory != nullptr &&
             tls_peer_verified &&
             verified_tls_transport != nullptr;
  }

  There is a sequencing issue: the initial SDP offer may be constructed before the TLS connection is established. Therefore:

  - secure configuration must bind the request to the TLS factory;
  - the URI must be sips:;
  - the SDES offer may be created before the handshake;
  - PJSIP must not transmit it until the selected TLS connection is authenticated;
  - a handshake or certificate failure must destroy the unsent transaction and clear the SRTP keys.

  Never retry the same INVITE over TCP.

  ## 4.3 Check incoming calls

  In IncomingRequest(), before reading or answering SDES SDP, require the received transport to be TLS:

  if (!PJSIP_TRANSPORT_IS_SECURE(
          data->tp_info.transport->key.type)) {
      RejectSecureInvite(data, PJSIP_SC_FORBIDDEN);
      return PJ_TRUE;
  }

  Also ensure the transport belongs to the expected TLS factory and satisfies the configured client-authentication policy.

  Then inspect SDP:

  TLS + RTP/SAVP + accepted crypto → continue
  TLS + RTP/AVP                 → reject downgrade
  TCP + RTP/SAVP                → reject; key arrived insecurely
  TCP + RTP/AVP                 → reject in secure-only mode

  A plain-TCP request containing an SDES key must not be passed deeper into the call/media setup path.

  ## 4.4 Check re-INVITEs and hold/resume

  The same rule applies to:

  - local hold
  - local resume
  - remote hold
  - remote resume
  - codec-changing re-INVITE
  - session refresh

  Before EncodeSdesOffer() or EncodeSdesAnswer():

  if (!ActiveDialogUsesVerifiedTls())
      return Error::security_failure;

  Before ActivateSdes():

  if (!ActiveDialogUsesVerifiedTls()) {
      StopCallMedia();
      ClearSrtpKeys();
      TerminateInvite();
      return;
  }

  This avoids a dialog beginning securely and later accidentally sending a re-INVITE over another transport.

  ## 4.5 Avoid TLS-to-TCP fallback

  Secure mode must reject:

  - registrar URI using sip:
  - transport=tcp
  - outgoing call URI using plain SIP
  - redirects to a plain SIP URI
  - Route headers that force TCP
  - reconnect logic that selects TCP after TLS failure

  URI validation should happen during ConfigureAccount() so invalid secure configuration fails early.

  A useful rule is:

  secure mode:
      account URI:   sips:
      registrar URI: sips: and transport=tls
      remote URI:    sips:

  ## 4.6 Error reporting

  TLS errors should not be reported generically as media failures.

  Add facade errors or status details for:

  tls_handshake_failure
  certificate_untrusted
  certificate_name_mismatch
  certificate_expired
  tls_transport_lost
  secure_transport_required

  If changing the public error enum would break compatibility, map them to the existing transport/authentication errors and expose
  detailed backend validation status separately.

  ## 4.7 Complete TLS plus SRTP validation

  The final application configuration should establish:

  Authenticated SIP TLS
         ↓
  REGISTER
         ↓
  INVITE containing strict SDES
         ↓
  RTP/SAVP
         ↓
  AES_CM_128_HMAC_SHA1_80 SRTP over UDP

  Run:

  1. valid TLS registration;
  2. outgoing TLS call with PCMU SRTP;
  3. hold/resume over TLS;
  4. remote re-INVITE over TLS;
  5. incoming TLS call;
  6. PCMA call;
  7. normal BYE;
  8. TLS transport loss;
  9. bad certificate;
  10. plaintext downgrade attempt;
  11. complete shutdown and resource check.

  The final acceptance output should be:

  PJLIB MBEDTLS RESULT: PASSED
  PJSIP TLS TRANSPORT RESULT: PASSED
  VOIP TLS+SRTP RESULT: PASSED

  ## Recommended implementation order

  Do not make one large patch. Use these increments:

  1. Add conditional PJLIB SSL configuration; no TLS enabled.
  2. Compile ssl_sock_mbedtls.c.
  3. Pass PJLIB loopback handshake and negative certificate tests.
  4. Compile sip_transport_tls.c.
  5. Pass PJSIP TLS OPTIONS validation.
  6. Add TLS credential ownership to the backend.
  7. Move registration to sips: and bind it to TLS.
  8. Move outgoing and incoming calls to TLS.
  9. add runtime verified-transport tracking;
  10. make SDES depend on and verify TLS;
  11. migrate the existing SRTP QEMU test to TLS;
  12. run full TLS+SRTP and plain Phase 7 regressions;
  13. qualify entropy, time, credentials, RAM and stack on MIMXRT1060.

  The critical completion condition is not merely that the TLS handshake succeeds. It is that no SDES key can be transmitted or
  accepted unless the exact SIP transport carrying it is authenticated TLS.