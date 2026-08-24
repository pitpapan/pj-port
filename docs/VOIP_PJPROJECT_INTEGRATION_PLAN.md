# PJPROJECT VoIP Integration Plan for Zephyr

## 1. Purpose

This plan covers product integration after the incremental PJLIB, PJLIB-UTIL,
PJSIP, and PJMEDIA port validations. It does not replace those port plans.
Their purpose is to prove individual library boundaries; this plan builds the
application-facing VoIP stack from those validated components.

The new implementation replaces both existing in-house components:

- the custom SIP signaling stack; and
- the custom RTPAudio media stack.

PJSIP owns SIP signaling, transactions, registration, dialogs, INVITE usage,
authentication, and SDP offer/answer. PJMEDIA owns codecs, RTP/RTCP, jitter
buffering, media streams, and media timing.

The application uses a small C++ facade. PJPROJECT APIs and object lifetimes
remain private implementation details.

## 2. Fixed Decisions

| Area | Initial decision |
| --- | --- |
| PJPROJECT version | 2.16, pinned in the repository |
| Development target | `mps2/an385` under QEMU |
| Product target | NXP MIMXRT1060 |
| Replacement boundary | Replace both custom SIP and RTPAudio implementations |
| Public interface | Existing-compatible C++ facade |
| Initial account limit | One configured account |
| Initial call limit | One simultaneous call |
| SIP transport | IPv4 TCP is mandatory |
| Media transport | IPv4 RTP and RTCP over UDP |
| Initial codecs | G.711 PCMU/PT 0 and PCMA/PT 8 |
| PCM boundary | Signed 16-bit, mono, 8 kHz; 20 ms frames initially |
| QEMU audio | Generated PCM source and bounded memory sink |
| Product capture | ADC/eDMA audio recorder |
| Product playback | SPI DAC audio player |
| High-level PJ API | Do not expose PJSUA2 to applications |
| Initial security/NAT scope | No TLS, SRTP, ICE, TURN, or IPv6 |
| Initial optional SIP scope | No presence, instant messaging, video, or conference bridge |

SIP over UDP may remain available to regression tests, but it is not a
substitute for the mandatory TCP production path.

## 3. Design Principles

### 3.1 Keep the application independent of PJPROJECT

Application headers must not require PJSIP or PJMEDIA types. This prevents PJ
pool ownership, callback threading, and version-specific structures from
becoming part of the SDK contract.

An internal implementation may use `pjsip_regc`, PJSIP dialogs and INVITE
sessions, PJMEDIA endpoints, media transports, and streams directly.

### 3.2 Use one shared runtime

Accounts and calls must not create independent PJ endpoints, timer heaps,
ioqueues, transports, or event threads. The first implementation owns:

- one PJLIB/PJSIP/PJMEDIA initialization lifecycle;
- one PJSIP endpoint;
- one event-processing thread;
- one SIP TCP transport factory;
- one account registration client;
- one active call; and
- one bidirectional media session.

### 3.3 Bound resources explicitly

The initial profile must use fixed-capacity account and call slots. Runtime
heap use is permitted during integration, but PJ pool peaks, system heap,
thread stacks, sockets, timers, transactions, dialogs, and media buffers must
be measured. Limits must not be reduced from the validated test profiles until
the integrated traffic profile has been measured.

The core facade should avoid exceptions and unbounded standard-library
containers. Credentials and URI strings must have documented ownership and
length limits.

### 3.4 Keep upstream protocol code intact by default

Do not modify PJSIP or PJMEDIA protocol sources merely to simplify the facade.
Use published PJPROJECT interfaces. A source patch is acceptable only when a
reproducible Zephyr compatibility defect is demonstrated, covered by a test,
and guarded so other operating systems retain upstream behavior.

### 3.5 Separate virtual and hardware validation

The `mps2/an385` QEMU target validates account management, SIP/TCP, calls,
SDP, G.711, RTP/RTCP, concurrency, failure recovery, and lifecycle behavior.
It does not emulate the MIMXRT1060 ADC/eDMA recorder or SPI DAC player.

Hardware audio is added only after the complete headless call works on QEMU.

## 4. Target Architecture

```text
Application
    |
    v
C++ VoIP facade
    |-- VoipManager: initialization, polling thread, and shutdown
    |-- SIPAccount: credentials, registration, refresh, and reconnect
    |-- SIPSession: incoming/outgoing call lifecycle
    `-- MediaSession: negotiated G.711 media state
             |
             v
        PJPROJECT 2.16
        |-- PJSIP: SIP/TCP, registration, dialog, INVITE, SDP
        `-- PJMEDIA: G.711, RTP/RTCP, jitter buffer, stream
             |
             |-- QEMU: generated PCM and bounded memory sink
             `-- Product: ADC/eDMA capture and SPI DAC playback
```

The facade may preserve the existing class names or supply compatibility
adapters. Application-visible behavior matters more than preserving internal
class layout.

## 5. Proposed Public API Boundary

The detailed API is finalized in Phase 1, but it should cover these concepts:

```cpp
struct SIPAccountConfig {
    const char *account_uri;
    const char *registrar_uri;
    const char *username;
    const char *password;
    unsigned registration_expires_seconds;
};

enum class SIPRegistrationState {
    disabled,
    registering,
    registered,
    unregistering,
    failed,
    connection_lost,
};

enum class SIPCallState {
    idle,
    outgoing,
    incoming,
    early,
    established,
    held,
    disconnecting,
    disconnected,
    failed,
};
```

The compatibility assessment must map the existing operations and callbacks,
including:

- registrable, direct outgoing, and direct incoming session creation;
- `OrderCall()`;
- `OrderAcceptCall()`;
- `OrderAbortCall()`;
- pending incoming call notification;
- established call notification;
- call end and session failure notification; and
- re-INVITE/hold notification.

Callbacks must contain stable copied values or facade-owned views. They must
not expose pointers into short-lived PJ pools.

## 6. Ownership and Threading Contract

`VoipManager` owns the complete PJPROJECT runtime. Initialization and shutdown
are explicit and repeatable. Destruction order is part of the API contract.

The event thread is the sole thread that directly advances PJSIP/PJMEDIA
events. Public operations invoked from other threads enqueue bounded commands.
Callbacks are serialized through one documented context; application code
must not be called while internal PJ locks are held.

The supported shutdown sequence is:

1. reject new public operations;
2. stop audio production and consumption;
3. stop and destroy the PJMEDIA stream and transports;
4. terminate and drain the active INVITE session/dialog;
5. unregister or destroy the registration client;
6. shut down TCP transports while event processing can drain callbacks;
7. unregister application modules and destroy the PJSIP/PJMEDIA endpoints;
8. destroy pools and the caching pool; and
9. shut down PJLIB.

All asynchronous callbacks must be drained or invalidated before their facade
objects are destroyed.

## 7. Account Management Design

The initial `AccountManager` has exactly one account slot. It wraps
`pjsip_regc` rather than implementing REGISTER or Digest authentication.

The account must support:

- disabled and enabled configuration;
- explicit register and unregister operations;
- Digest challenge handling through PJPROJECT credentials;
- automatic registration refresh;
- registration expiry reporting;
- SIP response status and reason reporting;
- TCP connection loss detection;
- bounded reconnect/backoff behavior; and
- deterministic destruction during registration or refresh.

The registration route must explicitly select TCP. Tests must verify the
actual PJSIP transport type and SIP Via/Contact transport semantics rather
than relying only on a URI resolver to choose TCP.

Credentials must never be written to normal logs. Configuration storage and
secure credential persistence are product responsibilities; the facade must
document whether it copies or references each value.

## 8. Call and Media Design

The initial call manager supports one call and these operations:

- outgoing INVITE;
- incoming INVITE notification;
- delayed accept;
- reject;
- local cancel before answer;
- remote cancel before answer;
- local and remote BYE;
- hold and resume through re-INVITE; and
- teardown after SIP TCP loss or media failure.

Before generating an SDP offer, the media session allocates its RTP and RTCP
ports. SDP initially offers PCMU and PCMA only. A call starts its PJMEDIA
stream only after offer/answer succeeds and the dialog reaches the required
state.

The QEMU media source generates deterministic signed 16-bit mono PCM. The sink
stores bounded output statistics or hashes; it must not grow for the duration
of a call.

## 9. Incremental Execution Plan

Each phase must have a dedicated configuration overlay, deterministic runtime
result, validation document, and clean build command. A later phase must keep
the previous phase as a regression gate where practical.

### Phase 0 - Integration baseline and requirements freeze

Deliverables:

- record the exact PJPROJECT, Zephyr, west, SDK, compiler, CMake, and QEMU
  versions used by the current workspace;
- record current Git revision and worktree state;
- verify the completed PJLIB/PJSIP/PJMEDIA validation artifacts from clean
  builds;
- create a requirements matrix from this document and `voip_stack.md`; and
- mark unresolved product requirements without selecting them silently.

Exit gate:

- all fixed decisions in Section 2 are represented by tests or explicitly
  marked as future hardware/product gates;
- no product implementation is started in this phase.

### Phase 1 - C++ facade contract

Deliverables:

- define public manager, account, session, observer, error, and state types;
- write the old-to-new API compatibility matrix;
- document string, credential, callback, and object ownership;
- document callback thread context and reentrancy restrictions;
- make headers compile in a minimal C++ Zephyr application without exposing
  PJPROJECT headers; and
- provide a fake backend for facade lifecycle tests where useful.

Exit gate:

- a C++ application can compile against the facade;
- the compatibility matrix accounts for every existing public operation used
  by the current product;
- facade construction and destruction require no PJPROJECT initialization.

### Phase 2 - Shared PJPROJECT runtime

Deliverables:

- implement one runtime owner for PJLIB, PJSIP, and PJMEDIA;
- create the endpoint, timer, ioqueue, pools, and one event thread;
- create the mandatory IPv4 TCP transport factory;
- implement a bounded cross-thread command queue;
- translate PJ errors into facade error values; and
- implement deterministic repeated initialization and shutdown.

Tests:

- initialize and shut down repeatedly;
- submit commands from the application thread;
- reject commands during shutdown;
- force partial initialization failures at each owned resource boundary; and
- verify no callbacks occur after destruction.

Exit gate:

- five complete runtime lifecycles pass on `mps2/an385`;
- all pools, timers, transports, threads, and command objects return to zero
  live ownership after every lifecycle.

### Phase 3 - Single-account model without networking

Deliverables:

- implement the fixed one-slot `AccountManager` and `SIPAccount` state model;
- validate configuration and bounded string copying;
- create and destroy a `pjsip_regc` without sending;
- install credentials through the PJPROJECT authentication API; and
- translate registration callbacks into the public observer contract.

Tests:

- invalid and oversized configuration;
- repeated configuration replacement;
- destroy while disabled, registering, registered, and unregistering using
  controlled callback injection; and
- observer removal during callback dispatch.

Exit gate:

- the account lifecycle has no network or media dependency;
- credentials never appear in logs or assertion messages.

### Phase 4 - SIP registration over TCP on QEMU

Deliverables:

- create an application-local deterministic registrar using a PJSIP TCP
  listener;
- send REGISTER using the facade account;
- handle a Digest challenge and authenticated retry;
- refresh before expiry;
- unregister with expiry zero;
- detect peer close and reconnect with bounded backoff; and
- verify all registration requests use TCP.

Tests:

- `REGISTER -> 401 -> authenticated REGISTER -> 200`;
- successful refresh;
- unregister;
- wrong credentials;
- timeout;
- registrar closes TCP before and after authentication;
- multiple SIP messages in one TCP receive stream;
- fragmented TCP messages;
- reconnect followed by successful registration; and
- shutdown during every registration state.

Exit gate:

- three complete register/refresh/unregister lifecycles pass on
  `mps2/an385`;
- packet and callback evidence proves TCP was used;
- no UDP SIP transport is required by the test.

### Phase 5 - One call over SIP TCP

Deliverables:

- implement the internal dialog/INVITE-session owner;
- add outgoing and incoming call facade operations;
- preserve the required existing observer behavior;
- keep registration active during the call;
- negotiate PCMU and PCMA SDP without starting media; and
- implement deterministic dialog and transaction teardown.

Tests:

- outgoing and incoming calls;
- provisional and final responses;
- delayed accept;
- local and remote CANCEL/487;
- reject/busy response;
- local and remote BYE;
- offerless INVITE;
- unsupported-codec rejection;
- TCP loss in early and confirmed states; and
- registration refresh while a call exists.

Exit gate:

- the complete call-control matrix passes over TCP on QEMU;
- only one configured call can exist and a second attempt fails cleanly.

### Phase 6 - Headless G.711 media facade

Deliverables:

- wrap the validated PJMEDIA G.711 endpoint, transport, and stream lifecycle;
- offer PCMU/PT 0 and PCMA/PT 8;
- use 8 kHz, mono, signed 16-bit PCM with 20 ms frames;
- implement the generated PCM source and bounded memory sink; and
- expose media state and statistics without exposing PJMEDIA types.

Tests:

- encode, RTP/UDP exchange, jitter buffering, decode, and sink verification;
- PCMU and PCMA independently;
- packet loss, reorder, duplicate, and malformed RTP/RTCP cases;
- pause and resume;
- RTP/RTCP transport shutdown during active streaming; and
- repeated stream creation and destruction.

Exit gate:

- both G.711 codecs cross the complete headless media path;
- buffers and pools remain bounded during an extended stream.

### Phase 7 - SIP-controlled headless call

Deliverables:

- allocate RTP/RTCP ports before creating SDP;
- bind negotiated addresses and codec selection to the media session;
- start media only when the SIP call state permits it;
- stop media before destroying the dialog; and
- coordinate account, call, and media callbacks through the facade.

Tests:

- registered outgoing and incoming calls over SIP TCP;
- bidirectional PCMU and PCMA media over RTP/UDP;
- CANCEL before media start;
- BYE during media;
- incompatible answer;
- SIP TCP loss during active media;
- RTP peer loss without SIP loss; and
- repeated complete register/call/media/unregister lifecycles.

Exit gate:

- a public-facade-only test performs the complete call lifecycle without
  directly invoking PJPROJECT APIs;
- generated PCM reaches both bounded sinks with the expected content.

### Phase 8 - Hold, resume, DTMF, and recovery

Deliverables:

- implement hold and resume through re-INVITE direction attributes;
- define RFC 2833 telephone-event behavior if required by the compatibility
  API;
- implement bounded registration reconnect and call-failure reporting;
- handle glare or overlapping re-INVITE deterministically; and
- define recovery behavior for partial media failures.

Exit gate:

- hold/resume pauses and restores the correct media directions;
- required DTMF behavior passes if enabled;
- recovery never leaves an account, dialog, media stream, timer, or transport
  alive after facade teardown.

### Phase 9 - QEMU robustness and resource qualification

Deliverables:

- run malformed SIP, SDP, RTP, and RTCP cases through the integrated facade;
- exercise controlled pool exhaustion and command-queue saturation;
- measure flash, static RAM, system heap, PJ pool peaks, stack watermarks,
  sockets, timers, transactions, dialogs, and media buffers;
- perform repeated lifecycle and active-call soak tests; and
- document supported limits rather than treating configuration maxima as
  proven capacity.

Minimum QEMU acceptance:

- at least ten complete initialize/register/call/teardown/shutdown cycles;
- at least one extended active media call;
- no assertion, deadlock, stale callback, unbounded growth, or credential
  leakage;
- one account and one call enforced cleanly;
- SIP TCP and RTP/RTCP UDP behavior explicitly observed.

This phase is the definition of done for the virtual integration target. It is
not the definition of done for the physical product.

### Phase 10 - MIMXRT1060 audio contracts

This phase begins only when the required hardware interfaces and board are
available. It does not require a virtual MIMXRT1060 machine.

Deliverables:

- document the ADC/eDMA recorder API, native sample rate, sample format,
  channel count, DMA buffer ownership, callback context, and overrun behavior;
- document the SPI DAC player API, native sample rate, queueing contract,
  callback context, and underrun behavior;
- define bounded adapters between each hardware API and the PJMEDIA PCM port;
- determine whether resampling is necessary; and
- define clock-drift and shutdown handling.

Exit gate:

- the contracts can be tested with fake recorder/player implementations on
  QEMU;
- no hardware-specific type enters the public SIP/account API.

### Phase 11 - MIMXRT1060 physical audio integration

Deliverables:

- implement ADC/eDMA capture and SPI DAC playback adapters;
- validate capture-only and playback-only paths before full duplex;
- connect the adapters to the already validated media session;
- measure CPU, latency, stack, heap, DMA use, overrun, and underrun; and
- validate repeated start/stop and error recovery on hardware.

Exit gate:

- a bidirectional G.711 call passes on the physical product board;
- audio remains stable for the agreed soak duration;
- hardware and PJ resources return to the baseline after every call.

### Phase 12 - Product interoperability and release gate

Deliverables:

- test against the selected real registrar/proxy and peer;
- validate credentials, refresh timing, TCP keepalive, reconnect, and server
  failure behavior;
- run the complete incoming/outgoing/hold/resume/teardown matrix;
- record audio quality and end-to-end latency;
- finalize memory and logging configurations; and
- document deliberately unsupported features.

Exit gate:

- every required behavior has a repeatable test and recorded result;
- no known blocker remains for the selected server, network, board, and audio
  hardware.

## 10. Build and Validation Convention

The exact overlay and build directory names are introduced by each phase. A
typical clean QEMU build follows this form:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 <integration-application> \
  -d <phase-build-directory> -- -DEXTRA_CONF_FILE=<phase-overlay.conf>
```

A bounded QEMU run follows this form:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
timeout --signal=TERM --kill-after=3s <seconds>s \
west build -d <phase-build-directory> -t run
```

Every validation record must include:

- exact commands executed;
- command exit status;
- build flash and RAM report;
- runtime pass/fail marker;
- expected timeout behavior, if QEMU enters idle after passing;
- measured resource peaks relevant to that phase;
- modified files; and
- remaining limitations.

## 11. Compatibility Policy

The migration target is behavioral and source compatibility where reasonable,
not preservation of the old implementation internals.

Compatibility adapters should preserve existing application flows while
allowing richer states and error reporting. Direct access to a raw PJPROJECT
handle may be provided only as an internal or explicitly advanced API; normal
application operation must not depend on it.

The old custom SIP and RTPAudio sources must not be linked into the same
production backend. During migration they may remain selectable as an
alternative backend, with build-time mutual exclusion.

## 12. Explicitly Deferred Decisions

These features are not part of the initial one-account, one-call G.711 target:

- TLS and certificate policy;
- SRTP;
- STUN, TURN, and ICE;
- IPv6;
- multicast RTP;
- codecs other than PCMU and PCMA;
- multiple simultaneous accounts or calls;
- presence and instant messaging;
- video;
- conference mixing;
- acoustic echo cancellation; and
- a general-purpose PJSUA/PJSUA2-compatible application API.

Adding any deferred feature requires a resource delta, failure analysis, new
configuration boundary, and validation phase. It must not silently enlarge the
initial supported profile.

## 13. Immediate Next Action

Begin with Phase 0 only. Confirm the current repository and toolchain baseline,
then record which existing validations are reusable. Do not start facade
implementation until the Phase 0 requirements matrix and baseline report are
complete.
