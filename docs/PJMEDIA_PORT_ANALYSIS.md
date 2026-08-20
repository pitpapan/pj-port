# PJMEDIA 2.16 Zephyr Port Analysis

## 1. Scope and evidence

This document analyzes the PJMEDIA work needed after the validated PJLIB,
PJLIB-UTIL, and PJSIP signaling port. The intended progression is:

1. SDP parsing and offer/answer negotiation;
2. PJSIP INVITE-session call control without RTP;
3. headless G.711 RTP/RTCP audio under QEMU;
4. a real Zephyr audio-device backend on the product board.

The analysis is based on PJPROJECT 2.16, as identified by
`PJ_VERSION_NUM_MAJOR=2` and `PJ_VERSION_NUM_MINOR=16`, at workspace revision
`0eed86bfe02fd8afe82514d9fd9050bea621af12`.

Evidence was taken from PJPROJECT's CMake and make targets, public headers,
production sources, and focused PJMEDIA tests. The Zephyr source tree was not
inspected. Zephyr remains an external dependency whose documented build,
network, timing, audio, and device interfaces must be used.

This analysis and the accompanying plan do not add PJMEDIA sources to the
build and do not claim that calls or media currently work.

## 2. Current validated baseline

The starting point is the completed QEMU signaling profile:

- PJPROJECT 2.16;
- Zephyr 4.4.0;
- West 1.5.0;
- Python 3.12.13 and CMake 4.4.2 from `.venv`;
- Zephyr SDK 1.0.1 with GCC 14.3.0;
- `mps2/an385` under QEMU;
- Picolibc and LLVM libc++;
- PJLIB select ioqueue with 32 handles;
- PJSIP core, dialogs/core UA, registration client, Digest authentication,
  resolver, UDP, and TCP;
- five complete signaling lifecycles and a 30.9-second Phase 11 soak.

The Phase 11 validation image used 255,280 bytes of flash and 602,904 bytes of
RAM. Its configured Zephyr heap was 393,216 bytes, with 290,384 bytes of PJ
pool blocks measured at peak. These numbers include validation code and are a
comparison baseline, not a promise that the current heap can hold a media
call.

## 3. Principal findings

### 3.1 PJMEDIA is several independently useful layers

PJMEDIA should not be enabled as one desktop-sized source list. Its useful
embedded boundaries are:

```text
SDP representation/parser/comparison
└── SDP offer/answer negotiation
    └── PJSIP INVITE sessions

PJMEDIA endpoint and codec manager
├── G.711 codec
├── RTP/RTCP and jitter buffer
├── media UDP transport
└── audio stream and media ports
    └── audio-device abstraction
        └── Zephyr product audio backend
```

Conference mixing, resampling, echo cancellation, file/AVI support, video,
SRTP, ICE, and most codecs are not prerequisites for the first audio call.

### 3.2 SDP parsing is smaller than SDP negotiation

The parser and representation boundary is directly implemented by:

```text
pjmedia/src/pjmedia/errno.c
pjmedia/src/pjmedia/sdp.c
pjmedia/src/pjmedia/sdp_cmp.c
```

`sdp.c` uses PJLIB-UTIL's scanner and otherwise relies on the already ported
PJLIB facilities. It has no socket, RTP, codec, audio-device, PJNATH, SRTP, or
video runtime requirement.

`sdp_neg.c` is different. Even with video disabled, it directly calls:

- `pjmedia_codec_mgr_get_dyn_codecs()`;
- `pjmedia_codec_info_to_id()`;
- `pjmedia_codec_mgr_find_codec()`;
- `pjmedia_stream_info_parse_fmtp()`; and
- `pjmedia_get_type()`.

Those functions are implemented by `codec.c`, `stream_common.c`, and
`types.c`. The upstream make build's `PJSDP_OBJS` list contains only
`errno.o`, `sdp.o`, `sdp_cmp.o`, and `sdp_neg.o`, but its link flags explicitly
include `libpjmedia`. Therefore the upstream four-object list is not proof
that SDP negotiation is a standalone four-source component.

The first Zephyr SDP-negotiation link closure should be tested with this
candidate group:

```text
pjmedia/src/pjmedia/errno.c
pjmedia/src/pjmedia/sdp.c
pjmedia/src/pjmedia/sdp_cmp.c
pjmedia/src/pjmedia/sdp_neg.c
pjmedia/src/pjmedia/codec.c
pjmedia/src/pjmedia/stream_common.c
pjmedia/src/pjmedia/types.c
```

This is a candidate to compile and audit, not yet a validated production
source list. `stream_common.c` also contains APIs outside FMTP parsing, so the
link audit must verify the behavior both with and without function-section
garbage collection. Unsupported functions must not be satisfied with stubs.
If a clean source boundary cannot be obtained without bringing in unrelated
runtime code, a small upstream-compatible helper refactor is preferable to a
dummy implementation, but it requires a separate decision before changing
PJPROJECT source.

### 3.3 INVITE support needs three PJSIP-UA sources initially

`pjsip-ua/sip_inv.c` directly uses PJMEDIA SDP and negotiation. It also calls
the 100rel and session-timer implementations even when an individual session
does not require those extensions. The initial INVITE source closure is:

```text
pjsip/src/pjsip-ua/sip_inv.c
pjsip/src/pjsip-ua/sip_100rel.c
pjsip/src/pjsip-ua/sip_timer.c
```

The application must initialize the 100rel and session-timer modules before
creating an INVITE session, followed by `pjsip_inv_usage_init()`.

These sources remain independently deferred:

| Source | Decision |
| --- | --- |
| `sip_replaces.c` | Optional Replaces extension; add after basic call lifecycle |
| `sip_siprec.c` | Optional session-recording extension |
| `sip_xfer.c` | Deferred; depends on PJSIP-SIMPLE event subscription |
| `sip_reg.c` | Already selected by the existing registration feature |

This lets INVITE, ACK, CANCEL, and BYE validation begin without PJSUA-LIB,
PJNATH, an audio device, or RTP.

### 3.4 A media endpoint can share the PJSIP ioqueue

`pjmedia_endpt_create2()` accepts an existing ioqueue and a worker count. The
initial profile should pass `pjsip_endpt_get_ioqueue()` and use zero PJMEDIA
worker threads. PJSIP's existing event pump then services both SIP and media
socket readiness through one validated ioqueue.

This avoids creating a second 32-handle ioqueue and makes shutdown ordering
observable. Multi-threaded polling remains a later scalability option.

The non-audio-device functions must initially use
`pjmedia_endpt_create2()`/`pjmedia_endpt_destroy2()`. The inline
`pjmedia_endpt_create()`/`pjmedia_endpt_destroy()` wrappers also initialize and
shut down the audio-device subsystem and should only be used after that
subsystem is present.

### 3.5 The first codec should be G.711 only

PCMU and PCMA are built into PJMEDIA's `g711.c` and use static RTP payload
types. They avoid third-party codec libraries and dynamic-payload negotiation,
making them the most deterministic first codec.

The G.711 implementation is not just `g711.c`. Its directly exercised support
includes:

```text
pjmedia/src/pjmedia/g711.c
pjmedia/src/pjmedia/alaw_ulaw.c
pjmedia/src/pjmedia/plc_common.c
pjmedia/src/pjmedia/wsola.c
pjmedia/src/pjmedia/silencedet.c
pjmedia/src/pjmedia/port.c
```

`alaw_ulaw_table.c` supplies a roughly 33 KiB conversion table when
`PJMEDIA_HAS_ALAW_ULAW_TABLE=1`. The embedded profile should initially test
the smaller algorithmic implementation with the table disabled, then compare
CPU use before making the product decision.

The first port should call `pjmedia_codec_g711_init()` directly. It does not
need to compile PJMEDIA-CODEC's `audio_codecs.c` or enable L16, GSM, Speex,
iLBC, G.722, Opus, or other codec families. The codec umbrella can be added
later if PJSUA-LIB requires its registration API.

G.711 uses PLC and silence detection. With `PJ_HAS_FLOATING_POINT=0`, WSOLA
selects its integer path. `stream.c` still contains two unconditionally
compiled `float` timestamp modifiers for G.722/Opus-related DTMF handling, so
the media-stream phase must audit compiler helper symbols and CPU cost even
though the initial G.711 path normally leaves the multiplier at one.

### 3.6 RTP and the audio stream are separate validation boundaries

The packet and runtime families should be added in this order:

| Family | Initial sources to audit |
| --- | --- |
| RTP/RTCP | `rtp.c`, `rtcp.c`, `rtcp_fb.c` |
| Jitter buffer | `jbuf.c` |
| Transport | `transport_loop.c`, then `transport_udp.c` |
| Stream information | `stream_common.c`, `stream_info.c` |
| Audio stream | `stream.c` |
| Endpoint/runtime | `endpoint.c`, `codec.c`, `event.c`, `format.c`, `types.c` |
| Test media ports/clock | `port.c`, `null_port.c`, selected memory ports, `clock_thread.c` |

The exact closure must be frozen from compile and undefined-symbol audits in
the phase that introduces each family. The entire upstream `pjmedia` target
must not be copied into the Zephyr module merely because these families live
in the same desktop library.

One RTP/RTCP UDP media transport normally consumes two sockets and two
ioqueue registrations unless RTCP multiplexing is deliberately configured and
validated. Socket, descriptor, network-context, poll-event, and ioqueue limits
must be measured with SIP signaling active at the same time.

### 3.7 The null audio backend does not generate media timing

PJMEDIA's `null_dev.c` implements device discovery, parameters, and stream
lifecycle, but its start function does not invoke capture or playback
callbacks. It can validate the audio-device API and PJSUA configuration, but
it cannot prove that audio frames flow.

QEMU media-flow validation therefore needs:

- a deterministic generated PCM source;
- a memory/hash/counting sink;
- an explicit PJMEDIA clock or synchronous test driver;
- known sample counts and RTP sequence/timestamp expectations.

This separates media correctness from product audio hardware.

### 3.8 A real Zephyr audio backend is product integration work

None of the existing ALSA, PortAudio, CoreAudio, WMME, Android, or Symbian
backends is appropriate for Zephyr. The real product needs a new
`pjmedia_aud_dev_factory` and `pjmedia_aud_stream` implementation using
documented Zephyr audio/device interfaces.

The least invasive design is a Zephyr-only backend compiled from the
PJPROJECT Zephyr module and registered with
`pjmedia_aud_register_factory()`. This avoids changing native Linux, Windows,
macOS, Android, and other PJPROJECT CMake paths. Automatic registration for a
future PJSUA-LIB profile can be added through a small Zephyr-only integration
wrapper after the explicit factory lifecycle is validated.

The backend must define and test:

- supported sample rates, initially 8 kHz mono signed 16-bit PCM;
- 10 ms or 20 ms frame cadence;
- full-duplex, capture-only, and playback-only behavior as required;
- bounded DMA/ring-buffer ownership;
- cache alignment/coherency on Cortex-M7;
- callback thread context and priority;
- underrun, overrun, start, stop, drain, and device-loss semantics;
- no allocation or unbounded blocking in steady-state audio callbacks.

QEMU cannot validate this backend. It belongs on `mimxrt1064_evk` with the
actual codec, clocks, pins, DMA, and board configuration.

## 4. Proposed Zephyr feature graph

The feature graph should keep compile boundaries explicit:

```text
CONFIG_PJPROJECT
└── CONFIG_PJLIB
    └── CONFIG_PJLIB_UTIL
        ├── CONFIG_PJSIP                         existing
        └── CONFIG_PJMEDIA
            ├── CONFIG_PJMEDIA_SDP
            │   └── CONFIG_PJMEDIA_SDP_NEG
            ├── CONFIG_PJMEDIA_ENDPOINT
            │   ├── CONFIG_PJMEDIA_G711
            │   ├── CONFIG_PJMEDIA_RTP_RTCP
            │   │   └── CONFIG_PJMEDIA_UDP_TRANSPORT
            │   └── CONFIG_PJMEDIA_STREAM
            └── CONFIG_PJMEDIA_AUDIODEV
                ├── CONFIG_PJMEDIA_AUDIODEV_NULL
                └── CONFIG_PJMEDIA_AUDIODEV_ZEPHYR

CONFIG_PJSIP_INVITE
├── CONFIG_PJSIP
└── CONFIG_PJMEDIA_SDP_NEG
```

The final symbol spelling is decided when Phase 1 is implemented, but the
dependency direction must remain. Production feature symbols belong in
`pjproject/Kconfig`; validation selectors belong only in the validation
application.

The important cross-branch constraints are:

| Feature | Required lower features |
| --- | --- |
| SDP negotiation | PJMEDIA SDP parser plus its audited helper closure |
| PJSIP INVITE | PJSIP core and PJMEDIA SDP negotiation |
| PJMEDIA endpoint | PJMEDIA base/SDP support and the existing PJLIB runtime |
| G.711 | PJMEDIA endpoint and codec manager |
| RTP/RTCP | PJMEDIA endpoint/runtime primitives |
| UDP media transport | RTP/RTCP, PJLIB sockets/ioqueue, IPv4, and UDP |
| Audio stream | endpoint, selected codec, RTP/RTCP, and media transport |
| Zephyr audio device | audio-device core plus documented Zephyr audio/device capabilities |

The Zephyr audio backend must additionally depend on the documented Zephyr
audio/device capabilities it actually uses. It must not silently enable a
board driver or select a particular board.

## 5. Configuration policy

The Zephyr profile should explicitly set these feature decisions inside the
existing Zephyr-only `pj/config_site.h` guard or a Zephyr-selected PJMEDIA
configuration header:

```text
PJMEDIA_HAS_VIDEO=0
PJMEDIA_HAS_SRTP=0
PJMEDIA_HAS_RTCP_XR=0
PJMEDIA_STREAM_ENABLE_XR=0
PJMEDIA_HAS_LEGACY_SOUND_API=0
PJMEDIA_RESAMPLE_IMP=PJMEDIA_RESAMPLE_NONE
PJMEDIA_HAS_SPEEX_AEC=0
PJMEDIA_HAS_LIBWEBRTC=0
PJMEDIA_HAS_LIBWEBRTC_AEC3=0
PJMEDIA_HAS_LIBYUV=0
PJMEDIA_HAS_FFMPEG=0
PJMEDIA_HAS_G711_CODEC=1
```

When PJMEDIA-CODEC headers or its umbrella registration source are used, all
unselected codec macros must also be set explicitly to zero. The defaults for
L16, GSM, Speex, iLBC, and G.722 are enabled in PJPROJECT 2.16 and are not an
acceptable embedded feature declaration.

When the audio-device subsystem is introduced, all host backend macros must
be explicitly zero in the Zephyr profile. `PJMEDIA_AUDIO_DEV_HAS_NULL_AUDIO`
is enabled only for its validation phase. A product Zephyr backend should use
its own Zephyr-only build selector rather than pretending to be a host
backend.

Starting embedded values to test are:

| Setting | Proposed initial value | Reason |
| --- | ---: | --- |
| `PJMEDIA_MAX_SDP_MEDIA` | 4 | one audio line with controlled expansion headroom |
| `PJMEDIA_MAX_SDP_FMT` | 16 | G.711 plus telephone-event with test headroom |
| `PJMEDIA_MAX_SDP_ATTR` | derived default | remains `2 * formats + 4` |
| `PJMEDIA_SDP_NEG_MAX_CUSTOM_FMT_NEG_CB` | 4 | no custom callback required initially |
| `PJMEDIA_MAX_MTU` | 1500 | retain normal Ethernet RTP ceiling |
| `PJMEDIA_MAX_MRU` | 2000 | retain upstream receive headroom initially |
| PJMEDIA worker threads | 0 | share the PJSIP event pump first |
| Concurrent media streams | 1 | first milestone is one audio call |

`PJMEDIA_CODEC_MGR_MAX_CODECS` is hard-coded to 32 in the public header rather
than protected by `#ifndef`. It should remain unchanged until measurements
justify an upstream source/ABI change.

## 6. Initial supported profile

The first headless-media milestone is deliberately narrow:

| Area | Initial decision |
| --- | --- |
| Media type | Audio only |
| Codec | PCMU and PCMA (G.711), 8 kHz, mono, 16-bit PCM |
| Packetization | 20 ms starting profile |
| Signaling | Existing IPv4 PJSIP UDP profile |
| Call control | INVITE, provisional response, 200, ACK, CANCEL, BYE |
| SDP | One audio `m=` line, RTP/AVP, sendrecv/inactive directions |
| Media transport | IPv4 UDP RTP plus RTCP |
| Security | Plain RTP for deterministic local validation only |
| Event model | Shared PJSIP/PJMEDIA ioqueue, one application event pump |
| QEMU audio | Generated PCM source and memory/hash sink |
| Hardware audio | Deferred to the product-board phase |
| Video | Disabled |
| SRTP/DTLS | Disabled |
| ICE/STUN/TURN | Deferred to PJNATH |
| Echo cancellation/resampling | Disabled |
| Conference bridge | Deferred |
| Files/AVI | Disabled |

Plain RTP is not a production security recommendation. SRTP/DTLS needs a
separate crypto, entropy, keying, interoperability, and resource milestone.

## 7. Lifecycle model

The initial SDP/INVITE-only lifecycle is:

```text
pj_init()
pjlib_util_init()
pj_caching_pool_init()
pjsip_endpt_create()
pjsip_tsx_layer_init_module()
pjsip_ua_init_module()
pjsip_100rel_init_module()
pjsip_timer_init_module()
pjsip_inv_usage_init()
create dialogs and INVITE sessions
run pjsip_endpt_handle_events()
destroy sessions/dialogs and drain callbacks
destroy modules and endpoint
pj_caching_pool_destroy()
pj_shutdown()
```

The headless-media lifecycle inserts:

```text
pjmedia_endpt_create2(pool_factory, pjsip_ioqueue, 0, ...)
pjmedia_codec_g711_init()
create media transport
create/start stream and generated PCM clock
...
stop/destroy clock and stream
close/destroy media transport
pjmedia_codec_g711_deinit()
pjmedia_endpt_destroy2()
```

All media callbacks and transport references must drain before codec, media
endpoint, PJSIP endpoint, pool factory, or PJLIB teardown.

The later audio-device lifecycle uses `pjmedia_aud_subsys_init()`, explicit
Zephyr factory registration, stream closure, factory unregistration, and
`pjmedia_aud_subsys_shutdown()` in matching ownership order.

## 8. Source classification outside the first profile

| Source family | Classification |
| --- | --- |
| `conference.c`, `conf_switch.c`, `conf_thread.c` | Mixing/routing; defer until multi-port product behavior is required |
| `resample_*.c`, `resample_port.c` | Defer; initial device and codec rates must match |
| `echo_*.c`, `echo_port.c` | Defer; requires measured full-duplex hardware audio behavior |
| `pjmedia/sound_legacy.c`, `pjmedia/audiodev.c` | Legacy compatibility wrappers; avoid for the new backend |
| WAV, AVI, and file player/writer sources | Filesystem/media-container features; not required |
| Video core, video codecs, and video devices | Explicitly out of initial scope |
| `transport_ice.c` | PJNATH dependency; later ICE track |
| `transport_srtp*.c` | SRTP/DTLS and crypto dependency; later security track |
| Third-party codecs and processing libraries | Add individually only after a product requirement |
| PJMEDIA test runner | Host test program; adapt focused cases into the Zephyr app |

The non-Zephyr source files remain in PJPROJECT. Explicit Zephyr source lists
determine what is compiled; cleanup must not delete other operating-system
backends.

## 9. Validation sources to adapt

Focused cases may be adapted from:

- `pjmedia/src/test/sdptest.c`;
- `pjmedia/src/test/sdp_attr_test.c`;
- `pjmedia/src/test/sdp_neg_test.c`;
- `pjmedia/src/test/rtp_test.c`;
- `pjmedia/src/test/jbuf_test.c`; and
- relevant session/stream logic after narrowing it to the selected profile.

The complete host runner, audio tools, MIPS tests, video tests, and codec
vectors for disabled codecs must not enter the production library.

## 10. Open gates

The following must be answered by implementation and runtime evidence:

1. What is the smallest reliable SDP-negotiation link closure without
   relying on unsupported functions being discarded?
2. Do reduced SDP maxima handle all product registrar/proxy and peer offers,
   including malformed boundary cases?
3. What stack peak results from worst-case SDP parsing and negotiation?
4. Can INVITE, CANCEL, ACK, BYE, re-INVITE, and offer/answer teardown repeat
   without stale dialog, transaction, timer, or negotiator state?
5. How many shared ioqueue handles and Zephyr socket contexts are consumed by
   SIP plus one RTP/RTCP transport?
6. Does G.711/PLC/WSOLA meet real-time cadence with the fixed-point profile?
7. Do `stream.c`'s float operations pull unexpected compiler runtime helpers
   or create unacceptable Cortex-M3/M7 cost?
8. What are flash, heap, pool, stack, and CPU deltas for SDP-only, INVITE,
   RTP transport, stream, and audio-device milestones?
9. What product audio sample formats and frame sizes are actually supported
   by the board codec and documented Zephyr driver?
10. How will Cortex-M7 DMA buffers handle alignment, ownership, and cache
    coherency?
11. What entropy, RTP port allocation, SSRC, sequence-number, and timestamp
    policy is acceptable for production?
12. Is plain RTP acceptable for the product, or must SRTP become a required
    later milestone?

None of these questions currently requires inspection of Zephyr
implementation source. They can be answered through documented interfaces,
build diagnostics, QEMU tests, and product-board measurements.
