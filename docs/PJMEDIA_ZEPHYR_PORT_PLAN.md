# PJMEDIA to Zephyr Port Plan

## 1. Objective

Extend the validated PJLIB/PJLIB-UTIL/PJSIP signaling port until a Zephyr
application can establish one audio call and exchange deterministic G.711 RTP
and RTCP.

The work has three explicit completion milestones:

1. **Call control:** SDP offer/answer plus INVITE, ACK, CANCEL, and BYE, with
   no RTP or audio-device claim.
2. **Headless media:** G.711 RTP/RTCP over IPv4 UDP under QEMU using generated
   PCM and a memory sink.
3. **Product audio:** full-duplex audio through a real Zephyr PJMEDIA audio
   backend on `mimxrt1064_evk`.

These milestones prevent an SDP compile, an accepted INVITE, and a real audio
call from being reported as the same result.

PJSUA-LIB/PJSUA2, PJSIP-SIMPLE, PJNATH, SRTP, TLS, video, additional codecs,
conference mixing, resampling, and echo cancellation are later tracks unless
a phase explicitly promotes one of them into scope.

## 2. Starting point

The baseline is documented in `docs/PJSIP_PHASE11_VALIDATION.md` and provides:

- PJPROJECT 2.16;
- Zephyr 4.4.0, West 1.5.0, SDK 1.0.1, and GCC 14.3.0;
- QEMU `mps2/an385` as the normal development target;
- PJLIB, PJLIB-UTIL, and PJSIP core signaling;
- IPv4 UDP/TCP transports, asynchronous SIP resolution, Digest registration,
  dialogs, transactions, and the core UA layer;
- a shared select ioqueue with a configured maximum of 32 handles;
- passing PJSIP Phase 11 lifecycle and resource validation.

The root PJPROJECT CMake entry point remains compatible with native operating
systems. Zephyr integration belongs in `pjproject/zephyr/CMakeLists.txt`,
module Kconfig, and Zephyr-guarded configuration. Other operating-system
sources remain present and unmodified unless a separately justified generic
fix is required.

Read `docs/PJMEDIA_PORT_ANALYSIS.md` before beginning implementation. Its
source boundaries and open gates are part of this plan.

## 3. Porting rules

- Do not inspect or modify the Zephyr source tree.
- Use documented Zephyr APIs, Kconfig, CMake, build diagnostics, and runtime
  evidence.
- Keep every production source list explicit; do not glob the PJMEDIA tree.
- Add one dependency family at a time and audit added objects and undefined
  symbols.
- Do not add dummy success functions to satisfy the linker.
- Do not enable all desktop PJMEDIA defaults through an umbrella header.
- Keep validation selectors and harness sources in the validation
  application, not the production module Kconfig/library.
- Keep unsupported host audio backends in PJPROJECT but out of the Zephyr
  source list.
- Keep board and device-tree choices out of generic PJMEDIA code.
- Treat compilation, link, call signaling, RTP packet flow, and audible audio
  as separate validation claims.
- Preserve the existing PJSIP signaling tests after every new library layer.
- No phase passes only because it compiles.

## 4. Initial feature profile

| Area | Initial decision |
| --- | --- |
| Media | Audio only |
| Codec | PCMU/PCMA G.711, 8 kHz mono, signed 16-bit PCM |
| Packet time | 20 ms starting value |
| SDP media lines | One audio line; limit starts with controlled headroom |
| Signaling | Existing IPv4 PJSIP profile |
| Call control | INVITE, 1xx, final response, ACK, CANCEL, BYE |
| RTP | RTP/AVP over IPv4 UDP |
| RTCP | Enabled; separate socket first |
| Media security | None in local validation; SRTP deferred |
| Event processing | Shared PJSIP/PJMEDIA ioqueue, one application pump |
| QEMU media source | Deterministic generated PCM |
| QEMU media sink | Frame counter/hash/sequence checker |
| Audio device | None until null-device lifecycle and product phases |
| Product target | `mimxrt1064_evk` after headless QEMU stability |
| Video, ICE, SRTP, files, conference, AEC | Disabled |

## 5. Target layout

Create an application independent of the existing PJSIP validation harness:

```text
applications/
└── pjmedia_minimal/
    ├── CMakeLists.txt
    ├── Kconfig
    ├── prj.conf
    ├── phaseN_*.conf
    └── src/
        ├── main.c
        └── phaseN_*.c
```

The existing `applications/pjsip_minimal` application remains the regression
baseline and should not accumulate media-only harness code.

Proposed production symbols are:

```text
CONFIG_PJMEDIA
CONFIG_PJMEDIA_SDP
CONFIG_PJMEDIA_SDP_NEG
CONFIG_PJSIP_INVITE
CONFIG_PJMEDIA_ENDPOINT
CONFIG_PJMEDIA_G711
CONFIG_PJMEDIA_RTP_RTCP
CONFIG_PJMEDIA_UDP_TRANSPORT
CONFIG_PJMEDIA_STREAM
CONFIG_PJMEDIA_AUDIODEV
CONFIG_PJMEDIA_AUDIODEV_NULL
CONFIG_PJMEDIA_AUDIODEV_ZEPHYR
```

Final spelling may be adjusted in Phase 1, but source ownership and dependency
direction must match the analysis.

## 6. Phase 0 - Freeze and reconfirm the signaling baseline

### Goal

Begin from a reproducible PJSIP result without changing PJPROJECT behavior.

### Actions

- record the workspace revision and PJPROJECT version;
- confirm `docs/PJSIP_PHASE11_VALIDATION.md` still describes the active
  configuration;
- perform a pristine PJSIP Phase 11 build;
- run it once under QEMU and require every existing pass marker;
- record flash, RAM, heap configuration, PJ pool peak, stack watermarks,
  timer peak, socket limits, and ioqueue limits;
- confirm no QEMU process remains;
- save the build log as the comparison baseline.

### Completion criteria

- the existing Phase 7, Phase 10, and Phase 11 gates still pass;
- the build is pristine and reproducible;
- resource numbers are recorded before any PJMEDIA object is introduced;
- no PJMEDIA source or configuration is changed in this phase.

## 7. Phase 1 - Add PJMEDIA configuration and build boundaries

### Goal

Create an off-by-default PJMEDIA module structure without yet claiming SDP
runtime behavior.

### Actions

- add the Kconfig dependency graph described in the analysis;
- add `${PJPROJECT_ROOT_DIR}/pjmedia/include` only when PJMEDIA is enabled;
- define explicit CMake source groups for SDP, negotiation helpers, endpoint,
  RTP/RTCP, stream, G.711, and audio devices;
- keep all groups empty or disabled except the compile boundary under test;
- map PJMEDIA feature decisions once through the Zephyr-only configuration
  path;
- explicitly disable video, SRTP, ICE, resampling, AEC, legacy sound, and all
  unselected codecs/backends;
- create `applications/pjmedia_minimal` with no PJMEDIA feature enabled by its
  default `prj.conf`;
- prove that disabling PJMEDIA adds no PJMEDIA object;
- prove that native/root PJPROJECT CMake files are unchanged.

### Configuration gates

The initial configuration must explicitly establish at least:

```text
PJMEDIA_HAS_VIDEO=0
PJMEDIA_HAS_SRTP=0
PJMEDIA_HAS_RTCP_XR=0
PJMEDIA_STREAM_ENABLE_XR=0
PJMEDIA_HAS_LEGACY_SOUND_API=0
PJMEDIA_RESAMPLE_IMP=PJMEDIA_RESAMPLE_NONE
PJMEDIA_HAS_G711_CODEC=1
```

When codec or audio-device umbrella headers are compiled, their host/default
features must also be set explicitly rather than inherited.

### Completion criteria

- PJMEDIA disabled reproduces the prior application boundary;
- each symbol has an explicit `depends on` relationship;
- no unsupported source appears in `build.ninja`;
- no native OS build path is changed;
- no runtime PJMEDIA claim is made.

## 8. Phase 2 - SDP representation, parser, and printer

### Goal

Validate SDP without negotiation, codecs, RTP, sockets, or a media endpoint.

### Initial source set

```text
pjmedia/src/pjmedia/errno.c
pjmedia/src/pjmedia/sdp.c
pjmedia/src/pjmedia/sdp_cmp.c
```

### Tests

- initialize PJLIB/PJLIB-UTIL and register PJMEDIA errors;
- parse valid one-line and multi-line SDP bodies;
- print and reparse sessions without semantic loss;
- clone sessions, media lines, connections, bandwidths, and attributes;
- compare identical and deliberately different sessions;
- validate IPv4 origin and connection fields;
- validate PCMU, PCMA, and telephone-event attributes;
- validate `sendrecv`, `sendonly`, `recvonly`, and `inactive`;
- reject missing mandatory fields, invalid payloads, bad line endings,
  truncated input, overlong input, excessive media lines, formats, and
  attributes;
- repeat parser initialization and pool destruction;
- measure stack and PJ pool peaks.

Adapt focused cases from `sdptest.c` and `sdp_attr_test.c`; do not import the
host test runner.

### Completion criteria

- all valid and invalid cases produce deterministic results under QEMU;
- boundary failures return controlled status values or documented parse
  errors;
- no socket, media endpoint, codec, RTP, or audio source is linked;
- repeated pool teardown leaves no live PJ allocations.

## 9. Phase 3 - SDP offer/answer negotiation

### Goal

Validate the smallest reliable SDP-negotiation closure before using INVITE.

### Candidate source additions

```text
pjmedia/src/pjmedia/sdp_neg.c
pjmedia/src/pjmedia/codec.c
pjmedia/src/pjmedia/stream_common.c
pjmedia/src/pjmedia/types.c
```

### Required audit

- record every object added by the symbol;
- audit undefined symbols in the resulting archive and final ELF;
- build with the normal Zephyr section-garbage-collection policy;
- perform an additional link probe that demonstrates supported APIs are not
  accidentally relying on unrelated discarded functions;
- if a clean closure requires additional real sources, add and classify them;
- if the boundary would require a PJPROJECT source refactor, stop and present
  the exact change before modifying upstream source.

### Tests

- local-offer and remote-offer creation;
- static PCMU/PCMA matching;
- telephone-event matching;
- codec ordering and direction negotiation;
- rejected media (`port 0`);
- no common codec;
- incompatible transport;
- malformed offer and answer;
- state transitions through local offer, remote offer, wait, done, modify,
  cancel, and renegotiation;
- repeated negotiator creation/destruction from pools;
- reduced SDP maximum boundary tests;
- stack and pool peaks.

### Completion criteria

- the supported SDP-negotiation APIs link without full PJMEDIA, PJNATH,
  audio-device, video, or codec-library targets;
- G.711 static payload offer/answer tests pass repeatedly;
- unsupported closure symbols are neither stubbed nor hidden;
- the exact validated source list is recorded in the analysis document.

## 10. Phase 4 - Compile, initialize, and destroy PJSIP INVITE support

### Goal

Add the minimum full-UA source closure but do not send a call yet.

### Source additions

```text
pjsip/src/pjsip-ua/sip_inv.c
pjsip/src/pjsip-ua/sip_100rel.c
pjsip/src/pjsip-ua/sip_timer.c
```

Keep `sip_replaces.c`, `sip_siprec.c`, and `sip_xfer.c` disabled.

### Tests

- initialize PJSIP core modules in the existing order;
- initialize 100rel, session timer, and INVITE usage modules;
- verify required callbacks are enforced;
- create and destroy UAC and UAS INVITE sessions with manually parsed SDP;
- exercise initialization failure cleanup;
- repeat the complete module lifecycle;
- verify parser registrations and global module state reset correctly after
  endpoint recreation;
- verify registration and OPTIONS regression tests still pass.

### Completion criteria

- the INVITE source group compiles and links only with the approved SDP
  closure and existing PJSIP libraries;
- modules initialize and destroy repeatedly;
- no RTP, media endpoint, PJNATH, PJSIP-SIMPLE, PJSUA, or audio-device object
  is linked;
- existing signaling behavior remains passing.

## 11. Phase 5 - Deterministic INVITE call control over loop transport

### Goal

Validate the dialog and INVITE state machines without network sockets.

### Tests

- create local UAC and UAS dialogs;
- send INVITE with a PCMU/PCMA offer over loop transport;
- send 100 Trying and 180 Ringing;
- negotiate a final SDP answer;
- send 200 OK and ACK;
- send BYE from each side and receive 200 OK;
- CANCEL before final response and validate 200/487 handling;
- reject with 4xx and 6xx responses;
- handle retransmission and timeout paths provided by the loop transport;
- test offerless INVITE followed by answer/offer exchange where supported;
- test re-INVITE, hold/inactive direction, and one failed renegotiation;
- terminate during each major INVITE state and drain all callbacks;
- repeat complete endpoint and call lifecycles.

### Completion criteria

- INVITE, ACK, CANCEL, and BYE state transitions match expected callbacks;
- SDP negotiators, dialogs, transactions, timers, and pools are released;
- no socket or RTP source is linked;
- the result is reported as **call control**, not a media call.

## 12. Phase 6 - INVITE call control over IPv4 UDP

### Goal

Validate one local SIP call over the already ported network transport while
media remains inactive.

### Tests

- use deterministic IPv4 loopback UAC/UAS services;
- complete INVITE/1xx/200/ACK/BYE with SDP bodies;
- test CANCEL and timeout;
- test retransmission after a deliberately dropped SIP datagram;
- test malformed SDP in requests and responses;
- test peer transport shutdown during early and confirmed dialog states;
- run registration, OPTIONS, and one call concurrently within configured
  limits;
- repeat call startup and teardown.

### Completion criteria

- call control passes over UDP without stale callbacks or dialog leaks;
- existing registration remains usable during call setup;
- resource deltas against Phase 5 are recorded;
- RTP and audio remain explicitly unclaimed.

## 13. Phase 7 - PJMEDIA endpoint, codec manager, and G.711

### Goal

Create the core media runtime and prove deterministic PCMU/PCMA conversion
without sockets or a running audio stream.

### Sources to audit

Begin with the real dependency families described in the analysis, including:

```text
endpoint.c
codec.c
errno.c
event.c
format.c
types.c
port.c
g711.c
alaw_ulaw.c
plc_common.c
wsola.c
silencedet.c
```

Do not add `alaw_ulaw_table.c` in the initial size-oriented profile. Do not
compile `pjmedia-codec/audio_codecs.c`; initialize G.711 directly.

### Tests

- create `pjmedia_endpt` with the PJSIP ioqueue and zero media workers;
- initialize and deinitialize the G.711 factory repeatedly;
- enumerate exactly PCMU and PCMA in the selected profile;
- allocate, open, modify, close, and deallocate codecs;
- encode known PCM to PCMU/PCMA and decode to expected samples;
- validate short buffers, invalid payload types, and invalid parameters;
- exercise PLC and VAD enabled/disabled behavior;
- compare algorithmic A-law/u-law conversion size and CPU cost;
- audit compiler runtime symbols caused by floating-point expressions;
- measure endpoint/codec pool and stack peaks;
- destroy G.711 before the media endpoint.

### Completion criteria

- endpoint and codec lifecycles pass repeatedly;
- deterministic vectors pass for PCMU and PCMA;
- only the intended codec is advertised;
- no socket, RTP stream, audio device, third-party codec, or PJNATH source is
  linked.

## 14. Phase 8 - RTP, RTCP, and jitter-buffer primitives

### Goal

Validate packet semantics independently of sockets and audio scheduling.

### Source families

- `rtp.c`;
- `rtcp.c`;
- `rtcp_fb.c` only where required by the selected stream information path;
- `jbuf.c`;
- any additional real dependency found by the object audit.

### Tests

- RTP session initialization and payload encoding/decoding;
- sequence wrap, timestamp wrap, SSRC change, duplicate, late, reordered,
  lost, malformed, and wrong-payload packets;
- RTCP sender/receiver reports and statistics;
- deterministic jitter/loss calculations;
- jitter-buffer put/get, prefetch, underflow, overflow, discard, reset, and
  sequence wrap;
- invalid packet length and header-extension bounds;
- repeated session destruction;
- fixed-point behavior and CPU cost.

### Completion criteria

- packet and jitter-buffer tests pass without sockets;
- malformed packets cannot overrun buffers or corrupt subsequent sessions;
- sequence, timestamp, loss, and jitter expectations are deterministic;
- resource and stack peaks are recorded.

## 15. Phase 9 - PJMEDIA loop and UDP transports

### Goal

Validate media transport callbacks and IPv4 RTP/RTCP sockets before creating
an audio stream.

### Actions and tests

- add and test `transport_loop.c` first;
- add `transport_udp.c` behind a networking-dependent Kconfig symbol;
- share the existing PJSIP ioqueue;
- create and destroy one RTP/RTCP transport;
- bind only to explicit IPv4 loopback addresses and ephemeral ports;
- exchange deterministic RTP and RTCP packets;
- validate attach/detach callbacks and user-data ownership;
- validate peer close, socket error, receive cancellation, and transport
  destruction while polling is quiesced;
- test RTCP source address and optional RTCP-mux behavior separately;
- reject malformed and oversized datagrams;
- run SIP registration and media sockets together;
- test ioqueue, socket, descriptor, context, connection, and poll limits;
- repeat transport lifecycles.

### Completion criteria

- loop transport passes before UDP is considered;
- RTP/RTCP UDP packet callbacks pass over loopback;
- media transport teardown produces no late callback;
- actual handle consumption and capacity failures are documented;
- ICE and SRTP transports remain absent.

## 16. Phase 10 - Headless G.711 audio stream

### Goal

Prove real-time media flow under QEMU without pretending the null device is an
audio source.

### Source families

- `stream_common.c`;
- `stream_info.c`;
- `stream.c`;
- selected media-port implementations;
- `clock_thread.c` or a synchronous deterministic clock path;
- the previously validated endpoint, codec, packet, jitter, and transport
  sources.

### Harness design

- generate a deterministic PCM waveform or sample sequence;
- packetize at 20 ms;
- send through two local media streams/transports;
- decode into a memory sink;
- count and hash captured frames;
- record RTP sequence, timestamp, SSRC, packet loss, jitter-buffer, and RTCP
  statistics;
- allow deliberate loss, duplication, delay, and reordering.

The PJMEDIA null audio backend may be added in a separate subtest for device
enumeration and stream start/stop only. It must not be used as proof of audio
frame flow because it does not drive callbacks.

### Tests

- bidirectional PCMU, then PCMA;
- exact packet cadence and timestamp increments;
- sustained generated audio;
- packet loss with PLC;
- jitter/reordering recovery;
- DTMF telephone-event send/receive;
- mute, pause, restart, and codec close/reopen;
- media transport shutdown during active stream;
- stream destruction with callbacks drained;
- stack, heap, pools, CPU load, and scheduling lateness.

### Completion criteria

- deterministic PCM reaches the sink through encode, RTP, jitter buffer, and
  decode;
- hashes/sample counts remain correct for the no-loss case;
- loss/reorder cases produce bounded documented behavior;
- the stream meets 20 ms cadence for the test duration;
- no hardware-audio claim is made.

## 17. Phase 11 - Integrated SIP call with headless RTP media

### Goal

Combine the validated call-control and media paths into one local audio call.

### Tests

- allocate RTP/RTCP ports before constructing the SDP offer;
- complete INVITE offer/answer using actual media transport addresses;
- start media only after the negotiated call state permits it;
- exchange bidirectional generated G.711 audio;
- verify RTCP statistics while SIP registration remains active;
- early media followed by final answer;
- CANCEL before media starts;
- remote BYE and local BYE while media is active;
- re-INVITE hold/resume and media-direction changes;
- rejected codec and changed remote RTP address;
- SIP timeout while media is active;
- orderly shutdown: stop media, destroy stream/transport, terminate dialog,
  drain events, then destroy endpoints and PJLIB;
- repeat complete call lifecycles.

### Completion criteria

- one full SIP-controlled bidirectional G.711 RTP call passes under QEMU;
- SDP addresses, negotiated payloads, stream state, and dialog state agree;
- shutdown is leak-free with no late callback;
- this phase may claim **headless media**, not audible product audio.

## 18. Phase 12 - Robustness and resource validation

### Goal

Determine the supported embedded limits of the headless media profile.

### Tests and measurements

- repeated complete signaling/media initialization and shutdown;
- long-running active call soak;
- repeated call setup/BYE and CANCEL cycles;
- malformed SDP, RTP, and RTCP boundary inputs;
- sustained loss, reordering, and burst traffic;
- socket/ioqueue exhaustion with SIP and RTP active;
- pool exhaustion and controlled allocation failure;
- timer, transaction, dialog, transport, stream, and callback counts;
- heap peak and steady-state PJ pool bytes;
- every thread stack high-water mark;
- flash/RAM delta for SDP, INVITE, packet, transport, stream, and integrated
  images;
- CPU utilization and maximum scheduling lateness at 20 ms cadence;
- production log level and credential/media-data logging policy.

### Completion criteria

- supported call and stream count is documented, initially one;
- exhaustion produces controlled errors rather than assertions or deadlocks;
- soak shows no unbounded pool growth or stale callback;
- timing meets the selected packet cadence;
- all prior PJSIP tests remain passing.

## 19. Phase 13 - Product-board build and network-media integration

### Goal

Cross-build and run headless signaling/RTP on `mimxrt1064_evk` before adding
the physical audio device.

### Actions and tests

- cross-build the same explicit source profile;
- resolve legitimate Cortex-M7/toolchain differences without board-specific
  PJMEDIA behavior;
- validate SIP plus RTP/RTCP against a controlled LAN peer;
- verify RTP port binding, link loss, address change, peer loss, and recovery;
- verify entropy and clock policy used for Call-ID, tags, SSRC, sequence, and
  timestamps;
- measure flash, static RAM, heap, stack, CPU, and packet timing on hardware;
- run a headless media soak;
- validate cache behavior for current network buffers before audio DMA is
  added.

### Completion criteria

- signaling and headless G.711 RTP operate on the product MCU/network;
- resource and timing budgets are recorded;
- no physical-audio claim is made.

## 20. Phase 14 - Zephyr audio-device backend

### Goal

Implement and validate real full-duplex product audio through PJMEDIA's audio
device API.

### Design requirements

- implement a real `pjmedia_aud_dev_factory` and `pjmedia_aud_stream`;
- keep the backend Zephyr-only and register it explicitly with
  `pjmedia_aud_register_factory()`;
- use documented Zephyr audio/device APIs without reading Zephyr source;
- expose only sample formats and rates the product can actually deliver;
- start with 8 kHz, mono, signed 16-bit PCM and 20 ms PJMEDIA frames;
- use bounded preallocated buffers;
- define ownership at every DMA/driver/PJMEDIA boundary;
- deliver PJMEDIA callbacks from safe thread context;
- avoid allocation and unbounded blocking in steady-state callbacks;
- handle capture-only, playback-only, and full-duplex modes if required;
- report unsupported capabilities with real PJ status codes;
- define volume, latency, underrun, overrun, drain, stop, and device-loss
  semantics;
- document Cortex-M7 cache-line alignment and cache-maintenance policy.

### Validation sequence

1. factory initialization, enumeration, parameters, and shutdown;
2. playback of a generated tone without SIP/RTP;
3. capture to a bounded memory sink without SIP/RTP;
4. local capture-to-playback loop with controlled gain;
5. PJMEDIA stream connected to the device without SIP;
6. one LAN SIP call with PCMU;
7. one LAN SIP call with PCMA;
8. start/stop, mute, underrun, overrun, link loss, and repeated-call tests;
9. full resource, latency, CPU, and audio-quality measurements;
10. sustained hardware call soak.

### Completion criteria

- capture and playback callbacks run at the documented cadence;
- no DMA buffer is reused while owned by another layer;
- underrun/overrun recovery is controlled and observable;
- one full-duplex G.711 call is audible in both directions;
- repeated calls and a sustained soak pass without leak, deadlock, stale
  callback, or unbounded latency;
- the supported board, codec, rate, frame size, and resource limits are
  documented.

This is the completion gate for the initial **product audio** milestone.

## 21. Later expansion tracks

These require separate approval and validation.

### PJSUA-LIB and PJSUA2

PJSUA-LIB is an orchestration layer over PJSIP-UA, PJSIP-SIMPLE, PJMEDIA,
PJMEDIA-CODEC, PJMEDIA-AUDIODEV, and PJNATH. Do not add it merely because the
headless media milestone passes. First define how unsupported SIMPLE, NAT,
video, and extra codec features will be compiled out. PJSUA2 additionally
requires a deliberate C++ exception, runtime, allocation, and thread policy.

### PJNATH and ICE

Add STUN, TURN, ICE, NAT detection, candidate gathering, extra sockets/timers,
and address-change behavior as a separate network-resource milestone.

### SRTP/DTLS

Select a real crypto backend, entropy source, keying method, certificate/time
policy where applicable, and interoperability tests. Plain RTP validation is
not evidence for secure media.

### Additional codecs

Add one codec at a time based on product requirements and license, flash,
heap, stack, CPU, latency, and quality measurements. Opus, Speex, G.722, GSM,
iLBC, and third-party codecs must not be enabled through defaults.

### Conference, resampling, and echo cancellation

Add these only after the physical device format and latency are known. Echo
cancellation must be validated with the final acoustic path, not QEMU.

### PJSIP extensions

Replaces, SIPREC, transfer, SIMPLE/presence, session recording, and other UA
features remain independently selectable.

### Video

Video is a separate memory, CPU, codec, transport, and device port and is not
part of the audio completion definition.

## 22. Validation command pattern

Each phase that changes buildable code uses a pristine build directory:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh

CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phaseN -- -DEXTRA_CONF_FILE=phaseN_<name>.conf

CCACHE_DISABLE=1 timeout --signal=TERM --kill-after=5s 90s \
west build -d build-pjmedia-phaseN -t run
```

Long soak phases may use a larger documented timeout. Exit status 124 is
acceptable only if the application printed an unambiguous phase pass marker
before `timeout` terminated an idle QEMU process.

After each run, verify that no background QEMU process remains.

For every phase report:

- exact commands;
- clean or incremental build status;
- added objects and source boundary;
- compiler/linker diagnostics and undefined-symbol audit;
- runtime pass/fail output;
- runner/timeout exit status;
- flash and RAM;
- heap, pool, stack, socket/ioqueue, timer, and CPU measurements relevant to
  the phase;
- remaining background processes; and
- files added or modified.

## 23. Definition of done

### Call-control milestone

- SDP parsing, printing, comparison, and offer/answer negotiation pass;
- INVITE, provisional response, final response, ACK, CANCEL, re-INVITE, and
  BYE behavior pass;
- repeated dialog/negotiator/module teardown is clean;
- RTP and audio are not claimed.

### Headless-media milestone

- PJMEDIA source lists and Kconfig boundaries are explicit;
- only selected G.711 audio features are enabled;
- RTP, RTCP, jitter buffer, UDP media transport, and stream tests pass;
- generated PCM crosses encode, network, jitter, decode, and memory-sink
  boundaries under SIP call control;
- resource, timing, failure, soak, and teardown behavior are documented;
- PJSUA, PJNATH, SRTP, video, and hardware audio remain explicitly deferred.

### Product-audio milestone

- the headless profile passes on `mimxrt1064_evk`;
- a real Zephyr audio factory/stream is implemented through documented APIs;
- full-duplex PCMU and PCMA calls work on the product hardware;
- DMA/cache, callback context, buffer ownership, underrun/overrun, latency,
  CPU, heap, stack, and soak behavior are validated;
- a clean checkout reproduces the build and results.

Only the third milestone permits the statement that the initial PJMEDIA audio
port is complete for the product board.

