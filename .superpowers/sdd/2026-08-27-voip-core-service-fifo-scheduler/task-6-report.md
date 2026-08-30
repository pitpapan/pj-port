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
