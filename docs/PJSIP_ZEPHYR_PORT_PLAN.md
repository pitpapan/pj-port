# PJSIP to Zephyr Port Plan

## 1. Objective

Extend the validated PJLIB Zephyr port until a Zephyr application can use
PJSIP for SIP signaling.

The first production-oriented milestone is deliberately narrower than the
complete PJPROJECT stack:

- PJSIP core;
- IPv4;
- SIP message parsing and generation;
- endpoint and event processing;
- loop, UDP, and TCP transports;
- client and server transactions;
- digest authentication;
- a registration client, if its source-level dependency analysis confirms it
  can remain independent of PJMEDIA;
- deterministic QEMU tests on `mps2/an385`.

The first milestone does not include RTP, audio, codecs, calls through
PJSUA-LIB/PJSUA2, TLS, ICE, TURN, or real product hardware.

The final product remains NXP i.MX RT1064, but QEMU remains the normal
development target until the signaling stack is stable.

## 2. Starting Point

The current baseline provides:

- Zephyr 4.4.0;
- Picolibc;
- Zephyr POSIX APIs;
- a Zephyr module for PJPROJECT;
- PJLIB initialization and shutdown;
- PJLIB threads, synchronization, pools, timers, sockets, DNS wrappers, and
  select-backed ioqueue;
- passing Stage 8 core runtime validation;
- passing Stage 9 UDP/TCP and resolver validation;
- passing Stage 10 ioqueue validation under QEMU.

PJLIB Stage 11 product-board integration remains deferred. It is not a
prerequisite for beginning PJSIP work under QEMU.

Before PJSIP implementation starts, all required PJLIB port files, especially
`pjlib/include/pj/config_site.h`, must be tracked in version control so that a
fresh checkout reproduces the baseline.

## 3. Dependency Boundary

The work must follow the real PJPROJECT dependency direction:

```text
PJLIB (completed baseline)
└── PJLIB-UTIL subset required by PJSIP
    └── PJSIP core
        ├── registration client, if proven independently buildable
        ├── PJSIP-SIMPLE (later optional track)
        └── full PJSIP-UA
            └── PJMEDIA SDP/negotiation
                └── media, codecs, and devices (later)

PJSUA-LIB / PJSUA2
├── PJSIP core, SIMPLE, and UA
├── PJMEDIA and codecs/devices
└── PJNATH for STUN/TURN/ICE
```

This means that "PJSIP core works" must not be reported as "PJSUA2 calls
work." They are separate completion milestones.

The upstream CMake targets are useful as dependency documentation, but the
Zephyr build must not add the complete upstream tree. In particular:

- PJSIP core requires PJLIB and selected PJLIB-UTIL facilities;
- the upstream PJSIP target also names the SSL library, but TLS will be
  disabled and its transport source omitted initially;
- full `pjsip-ua` depends on PJMEDIA because INVITE sessions use SDP and SDP
  negotiation;
- PJSUA-LIB and PJSUA2 pull in PJNATH, PJMEDIA, codecs, and device libraries.

## 4. Porting Rules

The PJLIB port rules continue to apply:

- do not inspect or modify the Zephyr source tree;
- use documented Zephyr interfaces, Kconfig, CMake, build diagnostics, and
  runtime results;
- keep source lists explicit and avoid source globs;
- do not compile tests, command-line tools, servers, or unrelated PJPROJECT
  libraries into the production target;
- do not add dummy functions merely to satisfy the linker;
- map every compile or link failure back to the API contract that caused it;
- keep validation-only Kconfig symbols in the validation application, not in
  the PJPROJECT module Kconfig;
- keep production feature decisions in module Kconfig;
- do not introduce board-specific behavior into generic PJSIP code.

The existing non-Zephyr source files remain in PJPROJECT. The explicit Zephyr
source lists determine what is compiled.

## 5. Initial Feature Profile

The first profile is:

| Area | Initial decision |
|---|---|
| IP version | IPv4 only |
| SIP transports | Loop, UDP, then TCP |
| TLS transport | Disabled |
| SIP resolver | Numeric address first; deterministic DNS/SRV later |
| Authentication | Digest authentication |
| Registration | Added after core transactions and authentication pass |
| SIP extensions | Deferred unless required by core tests |
| SDP | Deferred to the PJMEDIA expansion track |
| RTP/media | Deferred |
| STUN/TURN/ICE | Deferred to PJNATH |
| PJSUA-LIB/PJSUA2 | Deferred |
| Filesystem | No new requirement for the initial signaling profile |
| Development target | `mps2/an385` under QEMU |
| Product target | `mimxrt1064_evk`, later integration gate |

The feature profile must be reflected once through Kconfig and then consumed
by CMake/configuration headers. It must not be independently redefined in
several places.

## 6. Phase 0 - Freeze and Reconfirm the PJLIB Baseline

### Goal

Begin from a reproducible PJLIB baseline without changing PJLIB behavior.

### Actions

- confirm all current port files are tracked;
- record the PJPROJECT revision;
- perform clean Stage 8, Stage 9, and Stage 10 builds;
- rerun Stage 10 once under QEMU;
- record flash, RAM, configured heap, dynamic thread capacity, socket limits,
  and `PJ_IOQUEUE_MAX_HANDLES`;
- document the current ARMv7/little-endian assumption in `os_zephyr.h`.

### Completion criteria

- a fresh checkout can reproduce the PJLIB build;
- Stage 8 through Stage 10 remain passing;
- no background QEMU process remains;
- the baseline resource numbers are recorded for later comparison.

No PJSIP or PJLIB-UTIL source is added in this phase.

## 7. Phase 1 - Analyze PJSIP and PJLIB-UTIL

### Goal

Define the exact production source set and API dependencies before modifying
the build.

### Investigate

For PJLIB-UTIL, classify:

- initialization and error registration;
- scanner and string helpers;
- MD5, HMAC-MD5, SHA-1, and base64;
- DNS packet parsing and asynchronous resolver;
- SRV resolution;
- unrelated CLI, HTTP, WebSocket, PCAP, JSON, and XML facilities.

For PJSIP core, classify:

- configuration and generated headers;
- URI and message parsing;
- message printing and multipart support;
- endpoint lifecycle;
- modules;
- timers and event processing;
- transport manager;
- loop, UDP, TCP, and TLS transports;
- resolver integration;
- authentication;
- transactions;
- dialogs and UA layer.

For the registration client, verify whether `pjsip-ua/sip_reg.c` and
`pjsip-ua/sip_regc.h` can be selected without the PJMEDIA-dependent INVITE
sources. Do not assume that the upstream library boundary is indivisible, and
do not split it without proving its compile, link, and lifecycle dependencies.

### Deliverable

Create `docs/PJSIP_PORT_ANALYSIS.md` containing:

- source classification;
- required and optional source files;
- header/configuration decisions;
- initialization and shutdown order;
- Kconfig dependency graph;
- expected socket, timer, thread, pool, and heap use;
- unresolved API and semantic questions;
- the exact first compile source list.

### Completion criteria

Every source in the proposed Zephyr build has a stated reason, and every
upstream PJSIP/PJLIB-UTIL source omitted from the build is classified as
optional, test-only, tool-only, server-only, or deferred.

## 8. Phase 2 - Create an Isolated PJSIP Application and Module Options

### Goal

Create a separate application for PJSIP development without disturbing the
PJLIB validation application.

### Target layout

```text
applications/
└── pjsip_minimal/
    ├── CMakeLists.txt
    ├── Kconfig
    ├── prj.conf
    └── src/
        └── main.c
```

### Module configuration

Add production module symbols with explicit dependencies:

```text
PJPROJECT
└── PJLIB
    └── PJLIB_UTIL
        └── PJSIP
```

Transport and later-library symbols must depend on the component that owns
them. Proposed names are subject to the Phase 1 analysis, but should follow the
shape:

- `CONFIG_PJLIB_UTIL`;
- `CONFIG_PJSIP`;
- `CONFIG_PJSIP_UDP_TRANSPORT`;
- `CONFIG_PJSIP_TCP_TRANSPORT`;
- later, `CONFIG_PJSIP_REGC`.

TLS must remain disabled rather than silently compiling a nonfunctional TLS
transport.

Validation selectors belong only in `applications/pjsip_minimal/Kconfig`.

### Completion criteria

- the application builds with PJLIB only;
- enabling PJLIB-UTIL invokes only its explicit Zephyr source list;
- enabling PJSIP establishes its dependency on PJLIB-UTIL and PJLIB;
- disabling each component removes its sources from the build;
- there are no machine-specific absolute paths in committed CMake files.

## 9. Phase 3 - Port the Required PJLIB-UTIL Subset

### Goal

Compile, link, initialize, and validate only the PJLIB-UTIL facilities required
by PJSIP core.

### Initial candidates

The Phase 1 analysis should confirm an initial set drawn from:

- `errno.c`;
- `scanner.c`;
- `string.c`;
- `md5.c`;
- `hmac_md5.c`;
- `sha1.c`;
- `base64.c`.

DNS-related sources should be added when resolver work begins, not merely
because they exist upstream:

- `dns.c`;
- `resolver.c`;
- `srv_resolver.c`.

CLI, telnet, HTTP, WebSocket, PCAP, JSON, XML, and STUN utilities remain out of
the initial source set unless a demonstrated PJSIP dependency requires one.

### Validation

- `pjlib_util_init()` succeeds repeatedly;
- scanner boundary and error behavior;
- string escaping/unescaping required by SIP URI parsing;
- known MD5 and HMAC-MD5 vectors;
- known base64 encode/decode vectors;
- invalid input does not corrupt memory;
- initialization and PJLIB shutdown ordering is correct.

### Completion criteria

The validation application can include `pjlib-util.h`, initialize the selected
subset, run deterministic tests, and shut down repeatedly without leaks,
assertions, or unresolved symbols.

## 10. Phase 4 - Compile and Link PJSIP Core

### Goal

Compile and link the explicit PJSIP core source set with TLS excluded.

### Source policy

Use the Phase 1 source list. The expected families include:

- configuration, errors, messages, URI, parser, and multipart;
- endpoint and utility functions;
- resolver interface;
- transport manager and loop transport;
- enabled UDP/TCP transport sources;
- authentication;
- transaction layer;
- dialog and core UA layer.

Do not compile:

- `sip_transport_tls.c` while TLS is disabled;
- PJSIP test runners as production sources;
- PJSIP-SIMPLE, full PJSIP-UA, PJSUA-LIB, or PJSUA2;
- PJMEDIA or PJNATH;
- host command-line applications.

### Configuration work

- provide an explicit Zephyr PJSIP configuration path;
- ensure `PJSIP_HAS_TLS_TRANSPORT` is false;
- keep IPv6 disabled initially;
- verify `PJSIP_MAX_PKT_LEN` and all endpoint/transport/transaction pool sizes;
- do not rely on upstream autoconf output produced for the host;
- make generated configuration reproducible in a clean Zephyr build.

### Completion criteria

- a Zephyr source can include `pjsip.h`;
- all selected PJSIP core objects compile;
- the application links without PJMEDIA, PJNATH, SSL, PJSUA-LIB, or PJSUA2;
- no unresolved function is replaced by a dummy success implementation.

Runtime endpoint initialization is validated in the next phase.

## 11. Phase 5 - Parser, Message, and Endpoint Validation

### Goal

Validate non-network PJSIP semantics before relying on transports.

### Tests

- initialize PJLIB and PJLIB-UTIL;
- create and destroy `pjsip_endpoint` repeatedly;
- parse SIP and SIPS URIs, name-address forms, parameters, and escaped values;
- reject malformed URIs and messages;
- parse representative request and response messages;
- print and reparse messages without semantic loss;
- create request, response, ACK, and CANCEL transmit data;
- validate multipart body parsing and printing if included;
- verify error strings and module registration;
- exercise digest calculation with deterministic test values;
- destroy all endpoint pools, timers, ioqueue registrations, and modules in the
  correct order.

Prefer adapting focused upstream PJSIP tests such as URI, message, multipart,
and transmit-data tests. Do not import the entire upstream host test runner.

### Completion criteria

All deterministic parser/message tests pass repeatedly under QEMU, and
endpoint create/destroy leaves PJLIB able to shut down cleanly.

## 12. Phase 6 - Event Loop and Loop Transport

### Goal

Validate endpoint polling, timers, transport callbacks, and transaction state
machines without involving the Zephyr network stack.

### Tests

- create the loop datagram transport;
- send an OPTIONS request and receive a generated response;
- initialize and use the transaction layer;
- validate UAC and UAS state transitions;
- validate provisional and final responses;
- validate retransmission timers and transaction timeout;
- cancel/destroy transactions while endpoint event handling is active;
- run `pjsip_endpt_handle_events()` from the intended application threading
  model;
- repeat endpoint and loop-transport creation/destruction.

### Completion criteria

The loop transport and transaction tests pass without sockets. This separates
PJSIP state-machine defects from network-stack defects.

## 13. Phase 7 - UDP SIP Transport

### Goal

Validate SIP over Zephyr IPv4 UDP loopback.

### Tests

- start and stop a UDP transport;
- bind to an explicit loopback address and an ephemeral port;
- exchange OPTIONS requests and responses;
- exercise client and server transactions over UDP;
- validate retransmission after a deliberately dropped packet;
- validate timeout when no peer responds;
- receive malformed and oversized datagrams safely;
- verify transport reference counting and shutdown;
- close/unregister transport while the event loop is polling;
- repeat transport startup and teardown;
- confirm that socket/ioqueue limits fail with explicit PJ status values.

The test must remain deterministic and must not depend on Internet access.

### Completion criteria

SIP request/response and transaction behavior works over UDP loopback and
survives repeated startup/shutdown without stale callbacks.

## 14. Phase 8 - TCP SIP Transport

### Goal

Validate SIP stream transport behavior over Zephyr IPv4 TCP loopback.

### Tests

- start and stop a TCP listener;
- asynchronous connect and accept;
- OPTIONS request/response exchange;
- partial reads and writes;
- multiple SIP messages in one TCP stream;
- `Content-Length` framing;
- connection reuse;
- peer close, reset, and reconnect;
- transaction timeout during disconnect;
- transport shutdown while endpoint polling is active;
- concurrent UDP and TCP transports within configured resource limits.

### Completion criteria

TCP framing, connection lifecycle, and transaction behavior pass repeatedly
without leaks, stale callbacks, or ioqueue assertions.

## 15. Phase 9 - SIP Resolution

### Goal

Add deterministic PJSIP resolver behavior after numeric-address transports are
stable.

### Actions and tests

- add only the required PJLIB-UTIL DNS/resolver sources;
- create and attach a PJSIP resolver to the endpoint;
- use a local deterministic DNS test responder rather than public DNS;
- test A record lookup;
- test SIP SRV ordering and port selection;
- test missing records and malformed replies;
- test timeout, retransmission, cancellation, and resolver destruction;
- test numeric-address fallback;
- confirm that IPv6/NAPTR behavior remains explicitly disabled or deferred if
  it is not validated.

### Completion criteria

PJSIP can resolve a SIP destination deterministically and transports use the
resolved address and port correctly.

## 16. Phase 10 - Usable Signaling and Registration Profile

### Goal

Demonstrate application-level SIP signaling rather than only isolated library
tests.

### Registration decision gate

If Phase 1 proves that `sip_reg.c` has no PJMEDIA dependency, introduce it as a
separate registration-client feature with an explicit source list and Kconfig
dependency. If that proof fails, stop and move registration into the later
PJMEDIA/PJSIP-UA track rather than adding stubs.

### Tests

- create a client endpoint;
- register with a deterministic local registrar;
- process a `401 Unauthorized` challenge;
- resend REGISTER with valid digest credentials;
- receive and validate `200 OK`;
- refresh registration before expiry;
- unregister cleanly;
- reject invalid credentials;
- handle registrar timeout and recovery;
- send OPTIONS and optionally MESSAGE using the core API;
- shut down while no transaction or timer callback still references destroyed
  application state.

### Completion criteria

A Zephyr application can initialize PJSIP, register through the selected API,
send and receive basic SIP requests, process events, unregister, and shut down
cleanly.

This milestone is the definition of the initial "usable PJSIP" port. It does
not include calls or media.

## 17. Phase 11 - Robustness and Resource Validation

### Goal

Determine whether the signaling profile is safe for an embedded application.

### Tests and measurements

- repeated complete initialization/shutdown cycles;
- long-running event-loop soak;
- repeated register/refresh/unregister cycles;
- concurrent transactions;
- transaction and transport cancellation during shutdown;
- malformed-message and boundary-length tests;
- pool exhaustion and allocation-failure behavior where practical;
- maximum simultaneous transports and sockets;
- thread stack high-water marks;
- heap peak and steady-state usage;
- flash/RAM delta against the PJLIB-only baseline;
- timer and ioqueue handle counts;
- log volume and production log-level policy.

### Completion criteria

The supported limits are documented, resource exhaustion produces controlled
errors, and the QEMU soak completes without assertions, deadlocks, stale
callbacks, or unbounded memory growth.

## 18. Phase 12 - Product Integration

This phase begins only after the QEMU signaling profile is stable.

### Actions

- cross-build for `mimxrt1064_evk`;
- resolve any legitimate Cortex-M7/toolchain differences without introducing
  board-specific PJSIP behavior;
- validate ENET bring-up through the product application;
- test UDP/TCP SIP against a controlled LAN registrar/proxy;
- verify DHCP/static-address and DNS integration as required by the product;
- measure real-board stack, heap, flash, and CPU usage;
- test link loss, address change, server loss, and reconnection;
- run registration and signaling soak tests;
- capture enough protocol logging to diagnose failures without exposing
  credentials in production logs.

### Completion criteria

The initial signaling profile operates on the RT1064 hardware within the
product resource budget.

## 19. Later Expansion Tracks

These tracks require separate approval and validation plans.

### PJSIP-SIMPLE

Add event subscription, presence, MWI, and related XML handling. This likely
adds PJLIB-UTIL XML facilities and new parser/lifetime tests.

### PJMEDIA SDP and full PJSIP-UA

Port the PJMEDIA SDP and SDP-negotiation subset before enabling INVITE session
support. Validate offer/answer, dialog lifecycle, CANCEL/BYE, provisional
responses, session timers, and forked-dialog behavior before adding RTP.

### PJNATH

Add STUN, TURN, ICE, NAT detection, and their additional socket/timer/resource
requirements. Keep UPnP disabled unless explicitly required.

### RTP, codecs, and devices

Port the required PJMEDIA transport, RTP/RTCP, jitter buffer, conference/media
clock, selected codecs, and finally the product audio device. Do not begin with
all codecs or video support.

### PJSUA-LIB and PJSUA2

Only after their dependencies are present, enable the higher-level C and C++
APIs. PJSUA2 also requires a deliberate C++ runtime, exception, thread, and
memory policy for Zephyr.

### TLS

Add SIP TLS as its own security milestone. It requires a selected PJLIB SSL
backend, entropy, certificate storage/validation, time policy, socket tests,
and resource measurements. Do not enable `sip_transport_tls.c` before that
backend is real and validated.

## 20. Validation Command Pattern

Each phase that changes buildable code should use a pristine build directory:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh

CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-pjsip -- -DEXTRA_CONF_FILE=<phase-config>.conf

timeout 60s west build -d build-pjsip -t run
```

The exact phase configuration names will be created with the application. A
timeout exit status is acceptable only after the application has printed an
unambiguous passing result and QEMU is confirmed terminated.

For every phase, report:

- exact commands;
- clean or incremental build status;
- compiler and linker diagnostics;
- runtime pass/fail output;
- timeout/runner exit status;
- flash/RAM usage;
- remaining background processes;
- files added or modified.

## 21. Definition of Done

The initial PJSIP Zephyr port is complete only when:

- PJLIB-UTIL and PJSIP have explicit Zephyr source lists and Kconfig
  dependencies;
- TLS, media, NAT traversal, and other unsupported features are explicitly
  disabled;
- endpoint create/destroy works repeatedly;
- parser, message, authentication, and transaction tests pass;
- loop, UDP, and TCP transport semantics pass;
- deterministic SIP resolution passes if enabled;
- registration and unregister behavior passes if included in the approved
  profile;
- resource limits and failure behavior are documented;
- a clean checkout reproduces the build and QEMU results;
- documentation clearly states that PJSUA/PJSUA2 and media remain deferred.

No phase may be declared passed solely because it compiles.
