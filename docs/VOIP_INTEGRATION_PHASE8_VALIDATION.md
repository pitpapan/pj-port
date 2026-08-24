# VoIP Integration Phase 8 Hold and Recovery Validation

Date: 2026-08-24

## Result

Phase 8 passes on `mps2/an385` under QEMU:

```text
VOIP INTEGRATION PHASE 8 RESULT: PASSED (hold/recovery lifecycle)
```

## Integrated behavior

`VoipManager::SetHeld()` now serializes a PJSIP re-INVITE on the shared event
thread. Local hold offers `a=sendonly`; the answer is `a=recvonly`. Resume
offers and answers `a=sendrecv`. Successful negotiation pauses or resumes both
directions of the existing PJMEDIA streams without releasing their bounded RTP
transport resources. Facade observers receive `held`/`established` call states
and inactive/send-receive media directions.

Only one local offer may be outstanding. A competing facade request returns
`busy`; if the first transaction completed before the next queued request, the
next valid state change proceeds normally. PJSIP's invite-session transaction
handling supplies the protocol response for an incoming overlapping offer.
Failed re-INVITEs clear the outstanding flag, retain the last confirmed hold
state, and report the SIP status as a negotiation failure.

Registration recovery remains bounded to the Phase 4 policy: three retries
with 250, 500, and 1000 ms delays. SIP TCP loss reports call failure and
terminates the invite session. An isolated RTP failure reports
`media_failure`, leaves the SIP call established, and permits clean BYE and
facade teardown; media is recreated for the next call rather than retried
indefinitely inside the failed call.

## RFC 2833 decision

The existing-compatible facade has no DTMF operation, so RFC 2833
`telephone-event` is not advertised or enabled in this phase. Adding it would
expand the public compatibility contract. If that contract later gains a DTMF
operation, payload negotiation, event duration, retransmission, and receive
callbacks require a separate gated change.

## Validation matrix

The Phase 8 boot covers:

- registered outgoing SIP over TCP with PCMU RTP over UDP;
- `sendonly` hold and `sendrecv` resume through re-INVITE;
- inactive and restored bidirectional media callbacks;
- deterministic handling of a second queued hold-state change;
- injected partial RTP transport failure while SIP remains established;
- subsequent local BYE, sequential PCMU/PCMA and incoming/outgoing calls;
- existing bounded registration reconnect and call-failure cases; and
- final unregister and shutdown with no PJ account, dialog, transaction,
  media stream, timer, or transport reported live by the validation probes.

## Build and runtime

```sh
source .venv/bin/activate
west build -p always -b mps2/an385 applications/voip_integration \
  -d build_phase8 -- \
  '-DEXTRA_CONF_FILE=phase5_call.conf;phase8_hold.conf'
west build -d build_phase8 -t run
```

Final footprint:

```text
FLASH:  304152 B / 4 MB
RAM:   2337592 B / 4 MB
```

The generated PCM source and bounded memory sink remain validation-only. The
eventual MIMXRT1060 ADC/eDMA and SPI DAC adapters are unchanged future work.
No upstream PJPROJECT protocol source was modified.
