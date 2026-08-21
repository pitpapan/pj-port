# PJMEDIA Phase 4 INVITE Lifecycle Validation

Date: 2026-08-21

## Result

The Phase 4 INVITE module lifecycle passed under QEMU on `mps2/an385`. Three
complete PJLIB/PJLIB-UTIL/PJSIP endpoint lifecycles initialized the transaction
layer, UA layer, 100rel module, session timer, and INVITE usage. Each lifecycle
created and terminated UAC and loop-delivered UAS INVITE sessions, destroyed
the loop transport and endpoint, and completed pool cleanup.

Phase 5 was not started. The Phase 4 implementation, runtime gates, and
available PJMEDIA/PJSIP regressions all passed.

## Scope

| Item | Value |
| --- | --- |
| Goal | Compile, initialize, create, terminate, and destroy INVITE support |
| Production symbol | `CONFIG_PJSIP_INVITE` |
| Validation selector | `CONFIG_PJMEDIA_PHASE4_INVITE_TEST` |
| Board | `mps2/an385` |
| Build | `build-stage4` |
| Previous regression | PJMEDIA Phase 3 SDP negotiation |
| Explicitly deferred | RTP, RTCP, endpoint, codecs, streams, audio, sockets, Phase 5 call control |

## Runtime evidence

Command:

```sh
timeout --signal=TERM --kill-after=5s 30s \
  west build -d build-stage4 -t run
```

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

The five initialization stops occurred after transaction-layer, UA, 100rel,
session-timer, and INVITE-usage initialization respectively. Every stop
destroyed the endpoint and reported zero checked-out caching-pool resources.

Final build footprint:

```text
FLASH: 228248 B / 4 MB (5.44%)
RAM:   387448 B / 4 MB (9.24%)
```

The QEMU trace also showed successful registration and cleanup of:

```text
mod-tsx-layer
mod-stateful-util
mod-ua
mod-100rel
mod-invite
```

Each UAC session transitioned to `DISCONNECTED`, released its dialog usage,
destroyed its dialog, and left an empty timer heap before endpoint destruction.

The loop test also delivered a real INVITE through `PJSIP_TRANSPORT_LOOP_DGRAM`;
the validation module created a UAS dialog and UAS INVITE session from the
endpoint-provided `pjsip_rx_data`, parsed the PCMU SDP offer, terminated the
UAS session, and shut down the loop transport.

## Production source closure

The Phase 4 PJSIP additions are exactly:

```text
pjsip/src/pjsip-ua/sip_inv.c
pjsip/src/pjsip-ua/sip_100rel.c
pjsip/src/pjsip-ua/sip_timer.c
```

The PJMEDIA closure remains the seven Phase 3 SDP/negotiation objects. No
PJSUA-LIB, PJSIP-SIMPLE, PJNATH, RTP, audio-device, or optional PJSIP-UA
source was added.

## Validation-only implementation

```text
applications/pjmedia_minimal/src/phase4_invite.c
```

The harness validates:

- three repeated PJLIB and PJLIB-UTIL initialization/shutdown cycles;
- deterministic PJSIP endpoint creation and module initialization order;
- the mandatory `on_state_changed` INVITE callback;
- UAC dialog and INVITE session creation and termination;
- loop-delivered UAS dialog and INVITE session creation from parsed PCMU SDP;
- loop transport shutdown and validation-module unregister;
- endpoint destruction and caching-pool cleanup after each lifecycle.

## Audits

The Phase 4 image contains exactly these new PJSIP archive members:

```text
sip_inv.c.obj
sip_100rel.c.obj
sip_timer.c.obj
```

The PJMEDIA archive retains the seven Phase 3 objects, and
`arm-none-eabi-nm -u build-stage4/zephyr/zephyr.elf` reports no undefined
symbols.

## Regression attempts

The regressions were rerun with the workspace Python 3.12 environment:

```sh
source /home/pchen/zephyrproject/.venv312/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1

west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase3 -- -DEXTRA_CONF_FILE=phase3_sdp_neg.conf
timeout --signal=TERM --kill-after=5s 15s \
  west build -d build-pjmedia-phase3 -t run
```

Result:

```text
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

The existing PJSIP Phase 11 regression was also built pristine and run:

```sh
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-pjsip-phase11 -- -DEXTRA_CONF_FILE=phase11_robustness.conf
timeout --signal=TERM --kill-after=5s 60s \
  west build -d build-pjsip-phase11 -t run
```

Result:

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

## Open gates

- [x] Exercise controlled initialization-stop cleanup at each module boundary.
- [x] Verify parser/module global state after endpoint recreation through three
  complete module lifecycles.
- [x] Audit actual PJSIP/PJMEDIA archive members and final ELF undefined symbols.
- [x] Rerun PJMEDIA Phase 3 and existing PJSIP signaling regressions.
