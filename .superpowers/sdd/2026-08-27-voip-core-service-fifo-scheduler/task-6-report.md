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

## Fix round 3

Coverage-only round; no RED claim is made. Added explicit scheduler traces
for immediate initiation/acceptance/media hold/resume/finish/cleanup and for
queued waiting, promotion-retained waiting, and acceptance. Added a complete
capacity test admitting two promoted calls on different agents plus five FIFO
entries, asserting 7/2/5 exhaustion and restoration, stale handles, and
teardown/cancel cleanup.

Coverage commit: `5bd78180d` (`test(voip): expand scheduler lifecycle coverage`).

Verification passed:

```text
CallStateMachineTest PASSED
CallSchedulerTest PASSED
HandlePoolTest PASSED
AgentRegistryTest PASSED
VoipEventQueueTest PASSED
OperationMailboxTest PASSED
```

Focused state/scheduler ASan/UBSan binaries passed with leak detection
disabled under the ptrace-backed harness. `git diff --check` passed.

## Fix round 2

Code commit: `3919e2f37` (`fix(voip): make scheduler effects explicit`).
API ergonomics follow-up: `1fdc07b2c` (`fix(voip): add explicit scheduler effect overloads`).

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

## Fix rounds 4/5

Coverage-only hardening; no RED claim is made. The full-capacity test now
asserts all seven handles are live immediately after admission, processes
every handle deterministically without silent skips, verifies terminal
transition snapshots retain the original handle before teardown invalidation,
and checks stale acceptance/cancel commands after every release. It retains
the final exact 7 logical / 2 promoted / 5 FIFO availability assertions.

Verification passed the focused state and scheduler tests, all four prior
Plan 2 host tests, ASan/UBSan focused binaries (with leak detection disabled
under the ptrace-backed harness), and `git diff --check`.
