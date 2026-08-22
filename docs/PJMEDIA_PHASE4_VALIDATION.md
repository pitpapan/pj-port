# PJMEDIA Phase 4 INVITE Lifecycle Validation

Date: 2026-08-22

## Result

Phase 4 passes on `mps2/an385` under QEMU.

The validation initializes the PJSIP transaction, UA, 100rel, session-timer,
and INVITE modules; creates and destroys UAC and UAS INVITE sessions; and
repeats the complete endpoint lifecycle three times. The UAS request is built
and processed entirely in memory. No INVITE is sent, no PJSIP transport is
started, and no network socket is opened by the Phase 4 harness.

Phase 5 was not started.

## Environment

| Item | Value |
| --- | --- |
| PJPROJECT | 2.16 |
| Zephyr | 4.4.0 |
| west | 1.5.0 |
| Zephyr SDK | 1.0.1 |
| Python | 3.12.13 from workspace `.venv` |
| CMake | 4.4.2 |
| Board | `mps2/an385` |
| Build directory | `build-pjmedia-phase4` |

## Build and runtime

Pristine build:

```sh
CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase4 -- -DEXTRA_CONF_FILE=phase4_invite.conf
```

Final runtime check:

```sh
timeout --signal=TERM --kill-after=5s 5s \
  west build -d build-pjmedia-phase4 -t run
```

The harness completes in less than one simulated second. The timeout then
stops the QEMU runner, so exit status 124 after the pass marker is expected.

Observed markers:

```text
[Phase 4] initialization stop 1 cleanup: PASSED
[Phase 4] initialization stop 2 cleanup: PASSED
[Phase 4] initialization stop 3 cleanup: PASSED
[Phase 4] initialization stop 4 cleanup: PASSED
[Phase 4] initialization stop 5 cleanup: PASSED
[Phase 4] lifecycle 1 teardown: PASSED
[Phase 4] lifecycle 2 teardown: PASSED
[Phase 4] lifecycle 3 teardown: PASSED
PHASE 4 RESULT: PASSED (3 complete INVITE module lifecycles)
```

Final footprint:

```text
FLASH: 220720 B / 4 MB (5.26%)
RAM:   346312 B / 4 MB (8.26%)
```

## Lifecycle coverage

The five partial-initialization tests stop after:

1. transaction-layer initialization;
2. UA-layer initialization;
3. 100rel initialization;
4. session-timer initialization;
5. INVITE-usage initialization.

Every initialization result is checked before the test may report success.
Each partial lifecycle destroys its endpoint and verifies that the caching
pool has no checked-out resources.

Each of the three complete lifecycles performs:

- PJLIB and PJLIB-UTIL initialization;
- endpoint and module initialization in the required order;
- parsing of PCMU, PCMA, and telephone-event SDP;
- UAC dialog and INVITE-session creation and termination without creating or
  sending an INVITE request;
- construction of an in-memory incoming INVITE with the public PJSIP message
  API;
- UAS transaction, dialog, and INVITE-session creation from that in-memory
  request;
- checked UAS termination and bounded transaction-timer draining;
- verification of zero live dialog sets, transactions, and timers;
- transmit-data release, endpoint destruction, caching-pool destruction, and
  PJLIB shutdown.

The small stack-local `pjsip_transport` value used while constructing the UAS
dialog supplies only request metadata required by the public API. It is never
registered with the transport manager, has no worker thread or socket, and is
never used to send data.

## Configuration and source closure

Effective Phase 4 configuration includes:

```text
CONFIG_PJMEDIA_SDP=y
CONFIG_PJMEDIA_SDP_NEG=y
CONFIG_PJSIP_INVITE=y
CONFIG_PJMEDIA_PHASE4_INVITE_TEST=y
# CONFIG_PJMEDIA_ENDPOINT is not set
# CONFIG_PJSIP_UDP_TRANSPORT is not set
# CONFIG_PJSIP_TCP_TRANSPORT is not set
```

The only Phase 4 additions to the PJSIP production archive are:

```text
sip_inv.c.obj
sip_100rel.c.obj
sip_timer.c.obj
```

The PJMEDIA production archive remains the Phase 3 seven-object closure:

```text
errno.c.obj
sdp.c.obj
sdp_cmp.c.obj
sdp_neg.c.obj
codec.c.obj
stream_common.c.obj
types.c.obj
```

The final ELF contains the required INVITE/100rel/timer APIs and does not
contain `pjsip_loop_start`. The following command produced no output:

```sh
arm-zephyr-eabi-nm -u build-pjmedia-phase4/zephyr/zephyr.elf
```

No PJSIP UDP/TCP source, PJMEDIA endpoint, RTP/RTCP, media UDP transport,
stream, audio-device, PJSUA, PJSIP-SIMPLE, or PJNATH source was added for this
phase.

## Regressions

PJMEDIA Phase 3 was rebuilt pristine and run:

```text
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

The existing PJSIP Phase 11 image was rebuilt pristine. It includes the Phase
7 UDP lifecycle, Phase 10 registration/OPTIONS lifecycle, and Phase 11
resource soak:

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

No QEMU process remained after any run.

## Repository hygiene

`build-pjmedia-phase4` is ignored by Git. Generated `CMakeCache.txt`,
`cmake.check_cache`, and `build_info.yml` files are not version-controlled.
The final whitespace check passes.

## Completion gates

- [x] Exactly three PJSIP-UA production sources are enabled conditionally.
- [x] All initialization and validation cleanup results are authoritative.
- [x] No INVITE is sent and no transport or socket is started by the harness.
- [x] UAC and UAS sessions are created and destroyed with public APIs.
- [x] Three complete lifecycles return dialogs, transactions, timers, and
  pools to baseline.
- [x] Phase 3 and existing PJSIP signaling/resource regressions pass.
- [x] Generated build artifacts are absent from version control.
- [x] Phase 5 was not started.
