# PJMEDIA Phase 9 Loop/UDP Transport Validation

Date: 2026-08-24

## Result

Phase 9 passes on `mps2/an385` under QEMU:

```text
PHASE 9 RESULT: PASSED (3 loop/UDP media transport lifecycles)
```

This phase validates PJMEDIA loop-transport callbacks and plain IPv4 UDP
RTP/RTCP transports while sharing a live PJSIP ioqueue. It does not create a
media stream, codec, audio device, ICE transport, or SRTP transport. Phase 10
has not been started.

The production change activates two existing PJPROJECT sources through
default-off Kconfig gates and the Zephyr module CMake file. No PJPROJECT C
source or native/root PJPROJECT build file was changed.

## Environment and entry gate

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Entry revision | `9bfffcbe3` |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Compiler | GCC 14.3.0 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Ninja | 1.10.1 |
| Board | `mps2/an385` |

No file under the Zephyr source directory was inspected or modified.

## Selected closure

`PJMEDIA_LOOP_TRANSPORT` adds:

```text
transport_loop.c
```

`PJMEDIA_UDP_TRANSPORT` depends on the loop gate and IPv4 UDP sockets and adds:

```text
transport_udp.c
```

The runtime profile explicitly leaves these disabled:

```text
PJMEDIA_G711
PJMEDIA_SDP_NEG
PJMEDIA_STREAM
PJMEDIA_AUDIODEV
PJSIP_INVITE
PJSIP_TCP_TRANSPORT
NET_IPV6
```

The profile also keeps the established compile-time exclusions for video,
SRTP, ICE, and RTCP XR.

## Final runtime build and execution

Build command:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase9 -- -DEXTRA_CONF_FILE=phase9_transport.conf
```

Final linked image:

```text
FLASH: 209572 B / 4 MB
RAM:   721752 B / 4 MB
```

Runtime command:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=5s 25s \
  west build -d build-pjmedia-phase9 -t run
```

The pass marker appeared before timeout returned 124 and stopped the idle
QEMU process.

Each of three complete PJLIB/PJSIP/PJMEDIA lifecycles validates:

- two independent PJSIP UDP transports on explicit IPv4 loopback addresses;
- successful local REGISTER and unregister transactions while media sockets
  use the same ioqueue;
- a PJMEDIA endpoint using that ioqueue with zero media worker threads;
- loop transport creation, `attach2`, legacy `attach`, per-user callback
  ownership, two-recipient fanout, receive disable, detach, and destruction;
- two UDP media transports constructed from four distinct ephemeral
  `127.0.0.1` RTP/RTCP sockets;
- deterministic RTP and RTCP delivery through their respective callbacks;
- the observed RTCP source port matching the peer RTCP socket;
- separate RTCP-mux routing over the peer RTP socket;
- callback-level rejection of a three-byte malformed packet;
- socket-boundary rejection of a `PJMEDIA_MAX_MTU + 64` datagram without
  calling the transport send API outside its documented precondition;
- stop, detach, socket close, deferred ioqueue-key reclamation, and a 50 ms
  no-late-callback window while the event pump is active;
- destruction in reverse dependency order and an empty caching pool.

The event loop tolerates the expected transient `EBADF` from select-based
polling when another thread closes a registered socket. Each final lifecycle
observed one such retry and then shut down normally.

## Capacity and resources

The runtime uses these explicit limits:

| Resource | Configured limit |
| --- | ---: |
| PJSIP endpoint ioqueue handles | 16 |
| Network contexts | 30 |
| Network connections | 40 |
| Open file descriptors | 48 |
| Poll descriptors | 40 |

Observed results:

| Measurement | Result |
| --- | ---: |
| Concurrent SIP ioqueue handles | 2 |
| Concurrent media transports | 7 |
| Concurrent media ioqueue handles | 14 |
| Total live ioqueue handles exercised | 16 |
| Additional bound sockets before ordinary failure | 28 |
| Capacity failure status | 120002 (`too many open files`) |
| Event stack | at most 5992 / 8192 B used |
| Event stack unused | 2200 B |
| Main stack | at most 14744 / 32768 B used |
| Main stack unused | 18024 B |

All seven capacity transports were closed, deferred unregistration was
drained, and a new two-socket media transport was created successfully. All 28
raw sockets were then closed and a new bound socket was created successfully.

The 30-context ceiling is the first raw-socket limit reached: two live SIP
sockets plus 28 additional bound sockets. Consequently the higher connection,
descriptor, and poll settings are documented but are not independently
exhausted in this profile. Deliberately requesting a 17th ioqueue handle is
also not performed because the debug build treats the configured maximum as a
caller precondition; reaching 16 live handles and proving release/recovery is
the safe boundary test.

Plain UDP has no connection-level peer-close event. The applicable Phase 9
peer-shutdown behavior is peer transport destruction followed by local receive
cancellation, close-race handling, and callback quiescence. Stream-level
remote shutdown remains deferred to Phase 10.

## Public-API link probe

Commands:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase9-link-probe -- \
  -DEXTRA_CONF_FILE=phase9_link_probe.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=2s 5s \
  west build -d build-pjmedia-phase9-link-probe -t run
```

It passed at 103952 bytes flash and 320456 bytes RAM and emitted:

```text
PHASE 9 LINK PROBE: PASSED (loop/UDP transport public closure retained)
```

Ordinary guarded calls retain these supported public entry points under
normal section garbage collection:

```text
pjmedia_loop_tp_setting_default
pjmedia_transport_loop_create
pjmedia_transport_loop_create2
pjmedia_transport_loop_disable_rx
pjmedia_transport_udp_attach
pjmedia_transport_udp_create
pjmedia_transport_udp_create2
pjmedia_transport_udp_create3
```

The final probe ELF resolves every symbol.

## Feature-disabled proof and object delta

Commands:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase9-disabled -- \
  -DEXTRA_CONF_FILE=phase9_disabled.conf
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
timeout --signal=TERM --kill-after=2s 5s \
  west build -d build-pjmedia-phase9-disabled -t run
```

Result:

```text
FLASH: 76052 B
RAM:   188696 B
PHASE 9 DISABLED: PASSED (no media transport objects)
```

The disabled archive contains exactly the 12 Phase 8 objects:

```text
errno.c sdp.c sdp_cmp.c endpoint.c codec.c event.c format.c types.c
rtp.c rtcp.c rtcp_fb.c jbuf.c
```

The enabled runtime and link-probe archives contain exactly those 12 plus:

```text
transport_loop.c transport_udp.c
```

The runtime adds 133520 bytes flash and 533056 bytes static RAM relative to
the disabled marker profile. This is a whole-profile delta, not the isolated
cost of two archive objects: it includes PJSIP registration, IPv4 networking,
larger network buffers, a 512 KiB heap, thread instrumentation, and the runtime
test harness.

## Archive, source, and final-link audit

Commands:

```sh
PJSDK_BIN=/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin
"$PJSDK_BIN/arm-zephyr-eabi-ar" t \
  build-pjmedia-phase9/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-ar" t \
  build-pjmedia-phase9-link-probe/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-ar" t \
  build-pjmedia-phase9-disabled/modules/pjproject/libpjmedia.a
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase9/zephyr/zephyr.elf
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase9-link-probe/zephyr/zephyr.elf
"$PJSDK_BIN/arm-zephyr-eabi-nm" -u \
  build-pjmedia-phase9-disabled/zephyr/zephyr.elf
rg -n '/pjproject/.*(transport_ice|transport_srtp|stream\.c|pjmedia-audiodev|pjnath|pjsua)' \
  build-pjmedia-phase9/build.ninja || true
```

All three final ELFs resolve every symbol. The disabled archive contains no
Phase 9 object. No media stream, audio-device, PJNATH, PJSUA, video, SRTP, ICE,
or third-party codec implementation is selected.

## Previous-phase regression

A pristine Phase 8 build and run passed unchanged:

```text
FLASH: 105044 B / 4 MB
RAM:   190056 B / 4 MB
PHASE 8 RESULT: PASSED (3 socket-free RTP/RTCP/jitter lifecycles)
```

Its PJMEDIA archive still contains exactly the 12 pre-transport objects. The
expected QEMU timeout occurred only after the Phase 8 pass marker.

## Completion gates

- [x] Loop transport passes before UDP transport testing begins.
- [x] RTP and RTCP callbacks pass over explicit IPv4 loopback sockets.
- [x] Attach/detach ownership and loop callback fanout pass.
- [x] RTCP source-address and separate RTCP-mux routes pass.
- [x] Malformed and oversized datagrams are rejected at safe public
  boundaries.
- [x] SIP registration and media sockets coexist on one ioqueue.
- [x] Stop/detach/destruction produces no late callback.
- [x] Actual handle occupancy, first capacity failure, release, and recovery
  are recorded.
- [x] Three complete lifecycles and both stack watermarks pass.
- [x] The public-API closure links under normal garbage collection.
- [x] The disabled archive contains no Phase 9 object.
- [x] Phase 8 still passes unchanged.
- [x] ICE, SRTP, streams, and audio devices remain absent.
- [x] No PJPROJECT C source or native/root build file was changed.
