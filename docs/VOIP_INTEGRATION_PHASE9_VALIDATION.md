# VoIP Integration Phase 9 QEMU Robustness and Resource Qualification

Date: 2026-08-24

## Result

Phase 9 passes the virtual-target acceptance gate on `mps2/an385` under QEMU.
Ten independent boots each completed initialization, authenticated TCP
registration, the complete incoming/outgoing call and failure matrix, active
G.711 RTP/RTCP over UDP, unregister, and facade shutdown:

```text
[Phase 9] lifecycle 1 robustness matrix: PASSED
VOIP INTEGRATION PHASE 9 RESULT: PASSED (complete lifecycle; active media soak)
```

Each boot performs one shared PJPROJECT runtime lifetime, matching the product
architecture. Every lifetime contains twelve sequential call/dialog teardown
cases. An attempted experiment that reinitialized all PJPROJECT globals for a
second complete runtime inside one boot exposed failed media progress followed
by an assertion during continued failure cleanup. That unsupported pattern is
not used by the product and is not claimed as capacity; calls and accounts do
not independently initialize PJPROJECT.

## Robustness coverage

The integrated profile adds:

- exact eight-command queue saturation, controlled `queue_full`, drain, and
  recovery through the production event thread;
- malformed/null facade account and destination input rejection;
- one-account/one-call enforcement, including outgoing overlap while an
  outgoing or incoming call is active;
- incompatible SDP rejection without media startup;
- a 250-frame initial active-media soak followed by repeated PCMU/PCMA,
  offered and offerless, incoming and outgoing media calls;
- SIP rejection, CANCEL, local/remote BYE, early and confirmed TCP loss,
  registration reconnect, and partial RTP failure cases inherited by the
  integrated call matrix; and
- final transaction/dialog drain plus `HasLiveResources() == false` on every
  boot.

The already-completed lower-level gates remain part of Phase 9 qualification:

- PJSIP Phase 11 rejects malformed and 3,999/4,000/4,001-byte boundary SIP,
  catches fixed-pool exhaustion, saturates the configured ioqueue, and passes
  a 30-second signaling soak.
- PJMEDIA Phases 8 and 12 cover malformed RTP/RTCP, payload/SSRC/sequence
  boundaries, duplicate/loss/reorder/burst behavior, jitter-buffer bounds,
  malformed SDP, stream failure, and callback/resource cleanup.

These use the same ported PJPROJECT libraries. The integrated facade adds its
ownership, serialization, call-limit, and final-teardown checks without
duplicating private protocol parsers in the C++ layer.

## Measured QEMU resources

The final instrumented image reported:

| Resource | Observed or configured value |
| --- | ---: |
| Flash | 332,932 B / 4 MiB |
| Static RAM image, including configured heap | 2,337,688 B / 4 MiB |
| Zephyr system heap | 2,097,152 B configured |
| PJ pool block allocation peak | 306,760 B |
| PJ pool blocks at active sample | 58 |
| Transactions at sampled peak | 1 |
| Active timers at sampled peak | 4 |
| Dialogs at sampled peak | 2 (facade call and in-process peer) |
| Main stack | at most 7,904 / 49,152 B used |
| Event stack | conservative 8,192 / 8,192 B watermark reported |
| Media sink | 8 frames configured and never exceeded |
| Media sockets | four UDP sockets: RTP/RTCP for two headless endpoints |
| Signaling | TCP listener plus the loopback TCP connection transports |

The event thread uses a dynamically allocated PJLIB stack. Zephyr's stack
space query reported zero unused bytes for that allocation even though the
run completed without a stack assertion; therefore no event-stack headroom is
claimed. The lower-level PJSIP Phase 11 statically instrumented equivalent
event work at 2,472 / 8,192 B. Keep 8,192 B until the production board supplies
a reliable watermark for dynamically allocated thread stacks.

The PJ number measures pool block sizes through the public factory allocation
callbacks. It excludes allocator metadata and unrelated Zephyr heap users.
The large QEMU heap also hosts the registrar, peer, both sides of the media
path, and validation instrumentation; it is not a MIMXRT1060 production
budget.

## Supported limits

This phase demonstrates, and only claims:

- one configured account;
- one concurrent call;
- one PCMU or PCMA audio media line at 8 kHz, 20 ms frames;
- SIP signaling over IPv4 TCP;
- RTP and RTCP over IPv4 UDP;
- one shared runtime/event thread per boot;
- twelve sequential call cases per runtime and ten qualified boot lifetimes;
- an eight-entry facade command queue with explicit backpressure; and
- an eight-frame validation memory sink.

Kconfig transaction, timer, socket, descriptor, and heap maxima are ceilings,
not claimed application capacity. TLS, IPv6, SRTP, ICE, multiple accounts,
multiple calls, additional codecs, and physical audio are outside this result.

## Build and execution

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/voip_integration \
  -d build_phase9 -- \
  '-DEXTRA_CONF_FILE=phase5_call.conf;phase9_robustness.conf'
west build -d build_phase9 -t run
```

The final ELF was executed ten times with a 25-second host timeout. Each run
printed the pass and resource markers before QEMU was terminated. Assertions
were enabled. No assertion, deadlock, stale callback, unbounded resource
growth, or credential text appeared in the ten accepted runs.

No upstream PJPROJECT protocol source was modified. Phase 9 defines completion
for the QEMU integration target; MIMXRT1060 audio and hardware resource
qualification begin in Phase 10.
