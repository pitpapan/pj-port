# PJSIP 2.16 Zephyr Port Analysis

## 1. Scope and evidence

This is the Phase 1 source and dependency analysis for extending the validated
PJLIB Zephyr port to PJSIP signaling. It is based on PJPROJECT 2.16 at the
Phase 1 starting revision `4e287b817ac0d565f29fdfa4c7859f3b9105f693`.

This phase does not add PJLIB-UTIL or PJSIP to the Zephyr build. Zephyr remains
an external platform dependency; no Zephyr implementation source was inspected.

The analysis used the PJPROJECT 2.16 CMake targets, public headers, production
sources, and focused test sources as dependency evidence. A native host build
was also used to validate the registration-client boundary.

## 2. Decisions

The initial signaling port will use these boundaries:

- PJLIB-UTIL begins with only error registration, scanner support, SIP string
  escaping, and MD5.
- PJSIP begins with the parser/message/endpoint core, loop transport,
  transactions, MD5 digest authentication, dialogs, and the core UA layer.
- UDP and TCP translation units are controlled independently and are added to
  runtime validation in that order.
- full asynchronous DNS and SRV resolution are disabled initially;
  numeric IPv4 addresses and the PJLIB `getaddrinfo()` fallback remain usable.
- TLS and Digest AKA are explicitly disabled and their source files are not
  compiled.
- MD5 is the only supported Digest algorithm in the initial no-OpenSSL profile.
- the registration client is independently selectable; it does not require
  PJMEDIA, PJSIP-SIMPLE, the INVITE session layer, PJSUA-LIB, or PJSUA2.
- PJMEDIA, media, PJSIP-SIMPLE, PJNATH, PJSUA-LIB, and PJSUA2 remain deferred.

The root PJPROJECT CMake build remains untouched. All future Zephyr source
selection belongs in `pjproject/zephyr/CMakeLists.txt`.

## 3. Dependency and feature graph

The proposed Zephyr-facing dependency graph is:

```text
CONFIG_PJPROJECT
└── CONFIG_PJLIB
    └── CONFIG_PJLIB_UTIL
        └── CONFIG_PJSIP
            ├── CONFIG_PJSIP_LOOP_TRANSPORT
            ├── CONFIG_PJSIP_UDP_TRANSPORT
            ├── CONFIG_PJSIP_TCP_TRANSPORT
            ├── CONFIG_PJSIP_RESOLVER       (later DNS phase)
            └── CONFIG_PJSIP_REGC           (registration phase)
```

The component symbols should use `depends on` relationships. They should not
silently enable unrelated Zephyr networking facilities. Transport symbols
also need the corresponding application/platform capabilities:

- UDP: sockets, IPv4, and UDP;
- TCP: sockets, IPv4, and TCP;
- full resolver: UDP plus the PJLIB-UTIL DNS subset;
- loop transport: no network interface or socket requirement.

There will be no TLS symbol in the initial profile. Adding one later requires
a real PJLIB SSL backend, entropy, certificate, clock, storage, and lifecycle
plan rather than merely compiling `sip_transport_tls.c`.

## 4. PJLIB-UTIL source classification

### 4.1 Required initial subset

| Source | Reason |
| --- | --- |
| `pjlib-util/src/pjlib-util/errno.c` | `pjlib_util_init()` and PJLIB-UTIL error strings |
| `pjlib-util/src/pjlib-util/scanner.c` | scanner and character-input sets used by SIP URI, header, message, multipart, and authentication parsers |
| `pjlib-util/src/pjlib-util/string.c` | URI escaping and unescaping used by message/URI parsing and printing |
| `pjlib-util/src/pjlib-util/md5.c` | MD5 Digest authentication and proxy branch generation |

`scanner.c` textually includes either `scanner_cis_bitwise.c` or
`scanner_cis_uint.c`. Those two implementation fragments must not also be
compiled as separate translation units. The current default
`PJ_SCANNER_USE_BITWISE=1` is appropriate because it reduces parser BSS use.

### 4.2 Deferred DNS subset

These sources are added together only when `CONFIG_PJSIP_RESOLVER` is enabled:

| Source | Reason |
| --- | --- |
| `pjlib-util/src/pjlib-util/dns.c` | DNS packet construction, parsing, and duplication |
| `pjlib-util/src/pjlib-util/resolver.c` | asynchronous DNS resolver using PJLIB timer heap and ioqueue |
| `pjlib-util/src/pjlib-util/srv_resolver.c` | SIP SRV resolution and address ordering |

`dns_dump.c` is diagnostic-only and is not required by the resolver.
`dns_server.c` is a server facility; a deterministic validation responder
should live in the test application rather than the production module.

### 4.3 Deferred cryptographic helpers

| Source | Classification |
| --- | --- |
| `base64.c` | not used by normal MD5 Digest; needed by the deferred AKA implementation and potentially other later protocols |
| `hmac_md5.c` | only needed by deferred AKAv2 in the inspected PJSIP core |
| `sha1.c`, `hmac_sha1.c` | not referenced by the initial PJSIP core profile |
| `crc32.c` | not referenced by the initial PJSIP core profile |

PJPROJECT 2.16's `sip_auth_client.c` deliberately exposes only MD5 when
OpenSSL is unavailable. Although SHA-256 and SHA-512/256 identifiers exist in
the public API, `pjsip_auth_is_algorithm_supported()` rejects them in this
profile. The initial port must test and document MD5 only; it must not claim
RFC 7616 SHA-256 support.

### 4.4 Other omitted PJLIB-UTIL sources

| Sources | Classification |
| --- | --- |
| `cli.c`, `cli_console.c`, `cli_telnet.c` | interactive CLI, unrelated to signaling core |
| `getopt.c` | host command-line parsing |
| `http_client.c`, `websock.c` | unrelated client protocols |
| `json.c`, `xml.c` | not used by PJSIP core; XML may return with PJSIP-SIMPLE |
| `pcap.c` | offline capture utility |
| `stun_simple.c`, `stun_simple_client.c` | old STUN facilities; NAT work belongs to the later PJNATH track |
| `symbols.c` | not part of the upstream CMake target and not a production translation unit for this port |
| `resolver_wrap.cpp`, `xml_wrap.cpp` | alternate wrapper/amalgamation units, not compiled with their C implementations |
| all `src/pjlib-util-test/*` | upstream host test program; focused cases may be adapted into the Zephyr validation application |

## 5. PJSIP source classification

### 5.1 Base source set for the first PJSIP compile

The exact initial `pjsip` Zephyr library source list is:

```text
pjsip/src/pjsip/sip_config.c
pjsip/src/pjsip/sip_multipart.c
pjsip/src/pjsip/sip_errno.c
pjsip/src/pjsip/sip_msg.c
pjsip/src/pjsip/sip_parser.c
pjsip/src/pjsip/sip_tel_uri.c
pjsip/src/pjsip/sip_uri.c
pjsip/src/pjsip/sip_endpoint.c
pjsip/src/pjsip/sip_util.c
pjsip/src/pjsip/sip_util_proxy.c
pjsip/src/pjsip/sip_resolve.c
pjsip/src/pjsip/sip_transport.c
pjsip/src/pjsip/sip_transport_loop.c
pjsip/src/pjsip/sip_auth_client.c
pjsip/src/pjsip/sip_auth_msg.c
pjsip/src/pjsip/sip_auth_parser.c
pjsip/src/pjsip/sip_auth_server.c
pjsip/src/pjsip/sip_transaction.c
pjsip/src/pjsip/sip_util_statefull.c
pjsip/src/pjsip/sip_dialog.c
pjsip/src/pjsip/sip_ua_layer.c
```

Reasons by family:

| Family | Sources and role |
| --- | --- |
| Configuration/errors | `sip_config.c`, `sip_errno.c` expose configuration and register PJSIP error text |
| Representation/parsing | `sip_msg.c`, `sip_uri.c`, `sip_tel_uri.c`, `sip_parser.c`, `sip_multipart.c` implement SIP messages, URIs, headers, multipart bodies, parsing, and printing |
| Endpoint/utilities | `sip_endpoint.c`, `sip_util.c`, `sip_util_proxy.c` implement endpoint lifecycle and request/response/forwarding helpers |
| Resolution facade | `sip_resolve.c` is required by the endpoint and transport manager even with full DNS disabled |
| Transport core | `sip_transport.c`, `sip_transport_loop.c` provide transport management and deterministic socket-free validation |
| Authentication | `sip_auth_client.c`, `sip_auth_server.c`, `sip_auth_msg.c`, `sip_auth_parser.c` implement normal Digest authentication |
| Transactions | `sip_transaction.c`, `sip_util_statefull.c` provide UAC/UAS transaction state machines and stateful response helpers |
| Core UA/dialog | `sip_ua_layer.c`, `sip_dialog.c` are the core dialog/UA layer; they are distinct from the PJMEDIA-dependent INVITE session library |

With `PJSIP_HAS_RESOLVER=0`, `sip_resolve.c` compiles out asynchronous
DNS/SRV calls and uses numeric conversion or PJLIB `pj_getaddrinfo()`. The DNS
headers are still required for opaque types, but the DNS object files are not.

### 5.2 Independently selected transport sources

| Symbol | Added source | Runtime phase |
| --- | --- | --- |
| `CONFIG_PJSIP_UDP_TRANSPORT` | `pjsip/src/pjsip/sip_transport_udp.c` | UDP loopback phase |
| `CONFIG_PJSIP_TCP_TRANSPORT` | `pjsip/src/pjsip/sip_transport_tcp.c` | TCP loopback phase |

The loop source stays in the base set because it provides deterministic
transaction testing without the network stack. UDP and TCP can compile during
the core compile phase, but their Kconfig-controlled source boundaries must be
validated by configuring them both off and on.

### 5.3 Explicitly omitted PJSIP-core sources

| Source | Reason |
| --- | --- |
| `sip_transport_tls.c` | TLS is unsupported; compiling an empty macro-guarded object would obscure the real feature boundary |
| `sip_auth_aka.c` | Digest AKA defaults off and requires base64, HMAC-MD5, and Milenage; normal MD5 Digest does not need it |
| `sip_auth_parser_wrap.cpp`, `sip_dialog_wrap.cpp`, `sip_endpoint_wrap.cpp`, `sip_parser_wrap.cpp`, `sip_tel_uri_wrap.cpp`, `sip_transport_wrap.cpp`, `sip_util_proxy_wrap.cpp`, `sip_util_wrap.cpp` | alternate wrapper/amalgamation units that include C implementations and would duplicate symbols |

The public `pjsip.h` umbrella still includes TLS and AKA declarations. Their
configuration macros are zero and no implementations are linked, so attempts
to call those unsupported APIs must fail at link time rather than silently
succeed through stubs.

### 5.4 Deferred libraries and programs

| Source tree | Classification |
| --- | --- |
| `pjsip/src/pjsip-simple/dialog_info.c`, `dlg_event.c`, `errno.c`, `evsub.c`, `evsub_msg.c`, `iscomposing.c`, `mwi.c`, `pidf.c`, `presence.c`, `presence_body.c`, `publishc.c`, `rpid.c`, `xpidf.c` | later SIMPLE/presence/event-subscription track; some files require PJLIB-UTIL XML |
| `pjsip/src/pjsip-ua/sip_inv.c`, `sip_100rel.c`, `sip_replaces.c`, `sip_siprec.c`, `sip_timer.c`, `sip_xfer.c` | full UA/INVITE features; `sip_inv.c` introduces PJMEDIA SDP/negotiation dependencies |
| `pjsip/src/pjsua-lib/pjsua_acc.c`, `pjsua_aud.c`, `pjsua_call.c`, `pjsua_core.c`, `pjsua_dump.c`, `pjsua_im.c`, `pjsua_media.c`, `pjsua_pres.c`, `pjsua_txt.c`, `pjsua_vid.c` | high-level API requiring PJSIP-UA, SIMPLE, PJMEDIA, devices, and PJNATH |
| `pjsip/src/pjsua2/account.cpp`, `call.cpp`, `endpoint.cpp`, `json.cpp`, `media.cpp`, `persistent.cpp`, `presence.cpp`, `siptypes.cpp`, `types.cpp`, and internal `util.hpp` | high-level C++ API with the same broad dependencies and additional C++ runtime policy |
| `pjsip/src/test/*` | upstream integrated host tests, not production sources |
| `pjsip/src/pjsua2-test/*` | host C++ test program |
| `pjsip-apps/*` | applications, tools, servers, samples, and test programs |

## 6. Registration-client decision gate

`pjsip/src/pjsip-ua/sip_reg.c` passes the independence gate and may later be
compiled behind `CONFIG_PJSIP_REGC`.

Evidence:

- its includes are limited to `pjsip-ua/sip_regc.h`, PJSIP-core headers, and
  PJLIB headers;
- it contains no PJMEDIA, PJNATH, PJSIP-SIMPLE, INVITE-session, PJSUA, or
  dialog-layer reference;
- it uses endpoint pools/timers, the transport selector, parser/message
  helpers, the authentication client, and the transaction send path;
- compiling `sip_reg.c` alone succeeded;
- its undefined-symbol audit contained only PJLIB and PJSIP-core symbols;
- a native link probe succeeded with `libpjsip`, `libpjlib-util`, and `libpjlib`
  only.

Registration therefore does not require the complete upstream `pjsip-ua`
library. It does require an initialized endpoint and transaction layer, an
active event-processing loop, at least one usable transport, and destruction
of the registration client before endpoint teardown.

The source is not part of the first PJSIP compile list. It is introduced only
after transport and authentication behavior pass, as required by the port
plan.

## 7. Header and configuration decisions

### 7.1 Include paths

The Zephyr targets need these public include roots:

```text
pjlib/include
pjlib-util/include
pjsip/include
```

The application should include `<pjlib-util.h>` and `<pjsip.h>` normally. No
headers should be copied into the application.

### 7.2 Zephyr configuration path

Zephyr builds currently select the dedicated PJLIB `PJ_ZEPHYR`/`__ZEPHYR__`
configuration path and do not define `PJ_AUTOCONF`. Consequently PJSIP does
not need a generated `sip_autoconf.h` for Zephyr. PJSIP configuration should
be mapped in `pj/config_site.h` inside the existing Zephyr-only guard.

The initial mapping must establish:

```text
PJSIP_HAS_TLS_TRANSPORT=0
PJSIP_HAS_DIGEST_AKA_AUTH=0
PJSIP_HAS_RESOLVER=<CONFIG_PJSIP_RESOLVER>
```

Numeric resource values should also come from Kconfig once and be mapped into
PJSIP macros there. Native Linux, Windows, and other PJPROJECT builds remain
unaffected because all mappings stay inside the Zephyr guard.

### 7.3 Initial embedded limits

Upstream desktop defaults are too large for the first embedded profile. In
particular, the defaults allocate for 1,023 transactions, 511 dialogs, and a
timer heap of 3,068 entries.

The first QEMU profile should start with these explicit limits:

| PJSIP macro | Initial value | Reason |
| --- | ---: | --- |
| `PJSIP_MAX_TSX_COUNT` | 31 | 2^n-1 hash-table shape; enough for deterministic concurrency tests |
| `PJSIP_MAX_DIALOG_COUNT` | 15 | dialogs are not the first milestone but core lifecycle remains testable |
| `PJSIP_MAX_TRANSPORTS` | 16 | below the validated PJLIB ioqueue ceiling of 32 |
| `PJSIP_TPMGR_HTABLE_SIZE` | 15 | 2^n-1 hash-table shape matching the initial transport budget |
| `PJSIP_MAX_MODULE` | 16 | sufficient for core modules without the deferred stacks |
| `PJSIP_MAX_PKT_LEN` | 4000 | retain upstream SIP message compatibility for initial parser/transport tests |
| `PJSIP_MAX_TIMER_COUNT` | 128 | covers the 92-entry derived minimum for 31 transactions and 15 dialogs with headroom |
| `PJSIP_MAX_NET_EVENTS` | 1 | matches the validated select ioqueue recommendation |
| `PJSIP_MAX_TIMED_OUT_ENTRIES` | 10 | retain upstream bounded work per event poll |

These are validation starting points, not production claims. Kconfig ranges
must prevent `PJSIP_MAX_TRANSPORTS` from exceeding
`PJ_IOQUEUE_MAX_HANDLES=32`, and robustness testing must verify controlled
failure at every configured limit.

## 8. Initialization and shutdown order

The intended single-threaded initial application lifecycle is:

```text
pj_init()
pjlib_util_init()
pj_caching_pool_init()
pjsip_endpt_create()
pjsip_tsx_layer_init_module()      when transactions are enabled
pjsip_ua_init_module()             when core dialogs are exercised
create loop/UDP/TCP transports as selected
create pjsip_regc                  only in the later registration phase
repeat pjsip_endpt_handle_events()
```

PJSIP creates an ioqueue and timer heap but does not create an event-loop
thread. The application owns the polling thread and initially calls
`pjsip_endpt_handle_events()` from one registered PJLIB thread. Multi-threaded
polling is deferred until single-threaded transport and shutdown behavior is
stable.

Shutdown order is:

```text
stop new application work
destroy/cancel registration clients and wait for callbacks to drain
shut down and release transport factories/transports
pjsip_ua_destroy()                 if explicitly initialized
pjsip_tsx_layer_destroy()          if explicitly initialized
pjsip_endpt_destroy()
pj_caching_pool_destroy()
pj_shutdown()
```

`pjsip_endpt_destroy()` stops and unloads remaining modules, destroys the SIP
resolver facade and transports, then destroys its ioqueue, timer heap, locks,
parser state, and endpoint pool. Explicit destruction before it makes test
ownership and late callbacks observable rather than relying on endpoint
cleanup to hide lifecycle mistakes.

Both `pjlib_util_init()` and endpoint error registration are idempotent for
the same registered callback/range, allowing repeated full lifecycle tests in
one process.

## 9. Expected resource and platform use

### 9.1 Pools and heap

Important upstream pool defaults retained initially are:

| Object | Initial/increment |
| --- | ---: |
| Endpoint | 16,000 / 4,000 B |
| Transport manager | 1,000 / 1,000 B |
| Receive data | 4,000 / 4,000 B |
| Transport | 512 / 512 B |
| Transmit data | 4,000 / 4,000 B |
| Transaction layer | 512 / 512 B plus two hash tables |
| Each transaction | 1,536 / 256 B |
| Core UA | 512 / 512 B |
| Each dialog | 4,000 / 4,000 B |
| Registration client | 1,024 / 1,024 B |
| TCP listener/connection | 512 / 512 B before growth |

The timer heap allocates arrays proportional to `PJSIP_MAX_TIMER_COUNT` from
the endpoint pool. Reducing the desktop timer/transaction/dialog limits before
the first endpoint runtime is therefore mandatory, not an optional
optimization.

The PJSIP application should begin with the Stage 10 PJLIB capacity profile
and a deliberately visible heap value. Any increase must be justified by
measured endpoint, transport, and transaction peaks rather than hidden in the
module Kconfig.

### 9.2 Sockets and ioqueue handles

| Facility | Expected use |
| --- | --- |
| Loop transport | no socket or ioqueue handle |
| One UDP transport | one UDP socket and one ioqueue registration; default one 4,000-byte receive-data pool |
| TCP listener | one listening socket/ioqueue registration; default one outstanding accept |
| Each TCP connection | one socket/ioqueue registration, one receive buffer, keep-alive timer, and transport pool |
| Full DNS resolver later | at least one UDP resolver socket/ioqueue registration plus query timers |

The combined endpoint maximum starts at 16 ioqueue handles and must remain at
or below the validated PJLIB maximum of 32. Zephyr socket context, connection,
file-descriptor, and poll capacities must be tested against the exact mix of
UDP, TCP listener, accepted connections, and resolver sockets.

### 9.3 Threads and stacks

PJSIP core, transports, transactions, and the PJLIB-UTIL resolver do not
create worker threads themselves. The initial application needs one event
thread; the validated 8,192-byte dynamic-thread stack is the starting point.
Parser, callback, authentication, and resolver stack high-water marks must be
measured before reducing it.

### 9.4 Time and randomness

Transaction, registration, keep-alive, and resolver scheduling use relative
timers and can work with the validated monotonic clock behavior. The current
QEMU wall clock reports an epoch without a configured RTC, so Date headers and
any future certificate validation cannot yet be treated as production-valid.

The current QEMU configuration uses a test random generator. PJSIP uses random
data for tags, branch IDs, Call-IDs, authentication cnonce values, and server
nonces. Deterministic QEMU tests may proceed, but usable authentication on a
product requires a real entropy policy. Logging must never expose credentials
or complete authorization responses.

## 10. Validation sources to adapt

Focused behavior may be adapted from these upstream tests without importing
their host runner:

- PJLIB-UTIL: `encryption.c` for MD5 vectors; scanner/string behavior should
  get smaller boundary-focused cases in the Zephyr application;
- PJSIP: `uri_test.c`, `msg_test.c`, `msg_err_test.c`, `multipart_test.c`, and
  `txdata_test.c` for parser/message phases;
- loop/transactions: `transport_loop_test.c`, `tsx_basic_test.c`,
  `tsx_uac_test.c`, and `tsx_uas_test.c`;
- UDP/TCP: `transport_udp_test.c` and `transport_tcp_test.c`;
- registration: `regc_test.c`, narrowed to deterministic local registrar
  behavior.

Test-only source must stay in `applications/pjsip_minimal`; it must not enter
the production PJPROJECT module library.

## 11. Open questions and required validation gates

The following questions remain runtime gates rather than reasons to add port
stubs:

1. Does endpoint creation fit the proposed 128-entry timer heap and application
   heap without pool exhaustion?
2. What is the measured stack high-water mark for worst-case malformed
   messages and nested parser callbacks?
3. Does repeated endpoint/parser initialization fully reset global parser and
   module state on Zephyr?
4. Does transport destruction remain safe while the application thread is
   blocked in endpoint event handling? PJLIB Stage 10 passed the underlying
   ioqueue semantics, but PJSIP reference counts and callbacks still need
   validation.
5. Does TCP keep-alive and peer-close behavior work without a platform socket
   `shutdown()` implementation? PJSIP TCP currently closes through
   PJLIB active sockets, which is promising but must be tested.
6. Which exact socket/context limits support simultaneous UDP, a TCP listener,
   multiple TCP connections, and later one resolver socket?
7. How much entropy and persistent clock support will the product provide for
   authentication and any future TLS work?
8. Is a 4,000-byte SIP packet ceiling sufficient for the product's headers and
   registration responses, and how should larger UDP/TCP inputs fail?

No item requires inspecting Zephyr implementation source at this stage. They
can be answered through documented configuration, compile diagnostics, QEMU
tests, and resource measurements.

## 12. Phase 2 handoff

Phase 2 may now create `applications/pjsip_minimal`, add the component Kconfig
graph, and extend only `pjproject/zephyr/CMakeLists.txt`. Its first build must
still contain PJLIB alone. The next build enables only the four-source
PJLIB-UTIL subset. PJSIP is enabled only after those two boundaries are shown
to add and remove exactly the expected objects.

No PJSIP phase should be declared passed merely because the objects compile.
