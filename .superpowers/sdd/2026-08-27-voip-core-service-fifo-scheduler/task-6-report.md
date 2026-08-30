# Task 6 report — strict FIFO scheduler and logical call contexts

Base: `70793c22b`
Initial commit: `a01af0325`
Follow-up commit: `85eb6c3d0` (`fix(voip): keep scheduler phases private`)

## Scope

Added a PJPROJECT/Zephyr-independent seven-context call pool, normative UML
business-state machine, two-lease scheduler, and shared five-entry FIFO. The
scheduler keeps FIFO/lease ownership in private `LogicalCallPhase`; public
state and hold reason come only from each context's `CallStateMachine`. Outgoing
URIs are copied into contexts and incoming records carry only an opaque runtime
token. Agent leases are reflected in `AgentContext::promoted_call`.

## TDD evidence

The focused tests were compiled before the production files existed. Both
compilations failed with the expected missing-header/missing-source errors.
After implementation:

```text
CallStateMachineTest PASSED
CallSchedulerTest PASSED
```

The earlier Plan 2 host tests also passed:

```text
HandlePoolTest PASSED
AgentRegistryTest PASSED
VoipEventQueueTest PASSED
OperationMailboxTest PASSED
```

The state and scheduler tests were additionally compiled with
`-fsanitize=address,undefined` and passed with leak detection disabled because
the execution harness is ptrace-backed.

`git diff --check` is clean. No Zephyr source was inspected.

## Notes

`ScheduledTransition` copies transition/snapshot data before a logical slot is
released, and `OnTeardownComplete()` reports cleanup separately with no second
terminal-event flag. The scheduler's `OnCapacityChanged()` examines only the
FIFO head and repeats after each promotion. Direct initiation timeout releases
immediately after producing its terminal transition; promoted terminated calls
retain their lease until `OnTeardownComplete()`.

## Fix round 1

Private-phase follow-up commit: `85eb6c3d0`.
Fix-round code/report commit: `2ce49e391`.

This round makes the public machine match the normative UML exactly: an
established call has only `hold` and `finish` exits, while rejection/timeout
requests from that projection are mapped by the scheduler to the valid
`finish` edge. It also gates outgoing admission and later promotion on
`RegistrationState::registered`, rejects acceptance of queued calls, requires
an `AgentRegistry&` at scheduler construction, and keeps logical phases
private to `CallContext`.

Automatic promotions are returned through bounded `SchedulerEffects` storage
(capacity two), including direction, runtime token, handle, and optional
answer-on-promotion acceptance data. Release paths return their primary
transition and all promotion effects without dropping or overwriting them.

Fix-round verification:

```text
CallStateMachineTest PASSED
CallSchedulerTest PASSED
HandlePoolTest PASSED
AgentRegistryTest PASSED
VoipEventQueueTest PASSED
OperationMailboxTest PASSED
```

Focused state and scheduler binaries also passed with ASan/UBSan enabled
(`ASAN_OPTIONS=detect_leaks=0`; LeakSanitizer is unavailable under the
ptrace-backed harness). `git diff --check` is clean, and no Zephyr source was
inspected.

## Fix round 2

Code commit: pending.

The scheduler now requires a fresh caller-owned `SchedulerEffects&` for every
admission, capacity-change, and release operation. Effects are cleared at the
start of each such operation; no pending or persistent effects live in the
scheduler, and prefilled output cannot block promotion. Non-promotion state
operations no longer accept irrelevant effects parameters.

Promotion validates signaling eligibility, FIFO-head validity, available
effect capacity, and answer-on-promotion acceptance before changing ownership.
Every automatic promotion is returned in bounded two-entry storage with its
handle, direction, opaque token, and optional acceptance transition. Queued
outgoing calls require registered agents, while registration loss blocks only
the FIFO head and never bypasses it. Established rejection/timeout requests
are mapped to the normative `finish` edge.

Additional tests cover queued timeout head removal and promotion, mixed
direction ordering, simultaneous agents, stale commands after reuse,
teardown-only lease release, prefilled effects, opaque token ownership, and
capacity restoration. Fix-round verification passed the two focused tests,
all four earlier Plan 2 host tests, ASan/UBSan focused tests (with leak
detection disabled under the ptrace-backed harness), and `git diff --check`.
