# Phase 5 — INVITE Call Control over Loop Transport

## Goal in plain language

Make one complete SIP call-control exchange through PJSIP's in-memory loop
transport:

```text
INVITE → 100 → 180 → 200 → ACK → BYE → 200
```

Then validate CANCEL, rejection, timeout, re-INVITE, hold, and teardown.

This phase uses no network socket and sends no RTP. Passing it means SIP call
control works; it does not mean an audio call works.

## Prerequisite

Phase 4 must pass repeated INVITE module, UAC, and UAS lifecycle tests. Do not
begin by changing production CMake.

## Production source changes

Normally none.

The PJSIP loop-datagram transport is already part of the validated
`PJSIP_CORE_SOURCES` group. Phase 5 should reuse it. If a new production source
appears necessary, stop and audit why Phase 4 plus the existing PJSIP core is
insufficient.

## Files to change

```text
applications/pjmedia_minimal/Kconfig
applications/pjmedia_minimal/CMakeLists.txt
applications/pjmedia_minimal/src/main.c
applications/pjmedia_minimal/phase5_loop_call.conf             new
applications/pjmedia_minimal/src/phase5_loop_call.c             new
```

Do not modify the old `applications/pjsip_minimal/src/phase6_loop.c`; use it as
a reference for the event pump, loop transport, delay, discard, failure, and
teardown patterns.

## Step 1 — Add the validation selector and overlay

Add `PJMEDIA_PHASE5_LOOP_CALL_TEST`. It depends on the Phase 4 production
boundary:

```text
PJSIP_INVITE
PJMEDIA_SDP_NEG
```

It must exclude PJSIP UDP/TCP and all later PJMEDIA runtime families.

Create `phase5_loop_call.conf` by starting from the Phase 4 overlay:

```ini
CONFIG_PJLIB_UTIL=y
CONFIG_PJMEDIA=y
CONFIG_PJMEDIA_SDP=y
CONFIG_PJMEDIA_SDP_NEG=y
CONFIG_PJSIP=y
CONFIG_PJSIP_INVITE=y

CONFIG_PJSIP_UDP_TRANSPORT=n
CONFIG_PJSIP_TCP_TRANSPORT=n
CONFIG_PJMEDIA_ENDPOINT=n
CONFIG_PJMEDIA_G711=n
CONFIG_PJMEDIA_RTP_RTCP=n
CONFIG_PJMEDIA_UDP_TRANSPORT=n
CONFIG_PJMEDIA_STREAM=n
CONFIG_PJMEDIA_AUDIODEV=n

CONFIG_PJMEDIA_PHASE4_INVITE_TEST=n
CONFIG_PJMEDIA_PHASE5_LOOP_CALL_TEST=y

CONFIG_HEAP_MEM_POOL_SIZE=393216
CONFIG_MAIN_STACK_SIZE=32768
CONFIG_INIT_STACKS=y
CONFIG_THREAD_STACK_INFO=y
```

The larger validation heap allows two dialogs, transactions, messages, and
failure cases. Replace it later with a measured requirement.

## Step 2 — Reuse the Phase 4 initialization

Copy the proven Phase 4 lifecycle structure into `phase5_loop_call.c` and add:

```c
pjsip_loop_start(endpoint, &loop_transport);
pjsip_transport_add_ref(loop_transport);
pjsip_loop_set_delay(loop_transport, 5);
```

Start one bounded endpoint event pump. The reference implementation pattern is
in `applications/pjsip_minimal/src/phase6_loop.c`.

Hold one explicit loop-transport reference until shutdown. Release it only
after calls and timers are quiescent.

## Step 3 — First checkpoint: transport and idle pump

Before creating a call, prove:

- loop transport starts;
- it belongs to the expected endpoint/transport manager;
- the event pump starts and stops;
- a scheduled endpoint timer fires;
- loop shutdown leaves no timer, transport reference, thread, or pool live.

Build and run:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase5 -- -DEXTRA_CONF_FILE=phase5_loop_call.conf

timeout --signal=TERM --kill-after=5s 30s \
  west build -d build-pjmedia-phase5 -t run
```

Do not proceed to dialogs until this checkpoint is stable.

## Step 4 — Implement the smallest successful call

Use one PJSIP endpoint and two local SIP identities. The loop transport returns
the outgoing request to the same endpoint, where the UAS path handles it.

Implement in this order:

1. Parse fixed local and remote SDP bodies containing PCMU, PCMA, and
   telephone-event.
2. Create the UAC dialog with `pjsip_dlg_create_uac()`.
3. Bind it to the loop transport using a `pjsip_tpselector` and
   `pjsip_dlg_set_transport()` so no resolver or socket transport is selected.
4. Create the UAC INVITE session with `pjsip_inv_create_uac()`.
5. Create and send the initial request with `pjsip_inv_invite()` and
   `pjsip_inv_send_msg()`.
6. In the incoming INVITE path, verify the request and create the UAS dialog
   with `pjsip_dlg_create_uas_and_inc_lock()`.
7. Create the UAS session with `pjsip_inv_create_uas()`.
8. Send 100 Trying with `pjsip_inv_initial_answer()`.
9. Send 180 Ringing and then 200 OK with `pjsip_inv_answer()`.
10. Let the UAC send ACK and verify both sessions reach confirmed state.
11. End the session with `pjsip_inv_end_session()` and send the resulting BYE.
12. Verify 200 OK and both sessions reach disconnected state.

Record every state transition in `on_state_changed`. Test completion should be
based on explicit state/callback counts, not delays alone.

## Step 5 — Add scenarios one at a time

Keep the successful call passing while adding:

1. BYE initiated by the UAC;
2. BYE initiated by the UAS;
3. CANCEL before the final response, including 200 for CANCEL and 487 for the
   INVITE;
4. 4xx rejection;
5. 6xx rejection;
6. discarded packets and transaction timeout using
   `pjsip_loop_set_discard()`;
7. immediate and delayed transport failure using `pjsip_loop_set_failure()`
   and `pjsip_loop_set_delay()`;
8. offerless INVITE, if the PJPROJECT 2.16 API path is supported cleanly;
9. re-INVITE with hold/inactive SDP;
10. one deliberately incompatible renegotiation with a controlled SIP error.

After each scenario, require dialogs, transactions, SDP negotiators, timers,
and transmit data to return to their baseline counts before starting the next
scenario.

## Step 6 — Teardown order

For each complete lifecycle:

1. stop creating new call operations;
2. terminate active INVITE sessions;
3. drain endpoint events until transactions and timers quiesce;
4. release dialog/session references and locks;
5. shut down the loop transport;
6. release the explicit transport reference;
7. stop and join the event pump;
8. destroy the endpoint;
9. release pools and destroy the caching pool;
10. call `pj_shutdown()`;
11. verify zero late callbacks and live PJ allocations.

Repeat at least three complete endpoint/call lifecycles.

## Step 7 — Required marker and audits

Final marker:

```text
PHASE 5 RESULT: PASSED (3 complete socket-free call lifecycles)
```

Audit that:

- the PJSIP production archive has no new Phase 5 object;
- PJSIP UDP and TCP transport sources are absent;
- the PJMEDIA archive remains the Phase 3 closure;
- no PJMEDIA endpoint, RTP, codec implementation, stream, audio device,
  PJNATH, PJSUA, or PJSIP-SIMPLE source is present;
- the final ELF has no missing required symbol;
- no QEMU process remains.

## Step 8 — Regressions

Rerun:

- Phase 4 INVITE lifecycle;
- Phase 3 SDP negotiation;
- existing registration and OPTIONS signaling validation.

## Done checklist

- [ ] Successful INVITE/100/180/200/ACK/BYE flow passes.
- [ ] CANCEL and 487 handling pass.
- [ ] Rejection, timeout, and transport-failure paths pass.
- [ ] Offer/answer, re-INVITE, and direction changes are deterministic.
- [ ] No socket transport or RTP source is linked.
- [ ] Every lifecycle leaves zero dialogs, transactions, timers, callbacks,
      pools, and transport references.
- [ ] Phase 4 and signaling regressions pass.
- [ ] Result is described as call control, not an audio call.
- [ ] Phase 6 was not started.
