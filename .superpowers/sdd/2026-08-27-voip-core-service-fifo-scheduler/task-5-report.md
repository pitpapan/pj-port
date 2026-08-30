# Task 5 report: bounded operations and command mailbox

Base commit: `932e166cf`

## TDD evidence

The initial focused compile was run before production files existed:

```text
fatal error: ../../src/core/CommandMailbox.hpp: No such file or directory
```

This was the expected RED caused by the missing Task 5 types. The test then
drove the implementation through GREEN.

## Delivered

- `VoipCommand` is a fixed, tagged value union for dial, answer, reject,
  cancel, hangup, set-held, and shutdown. URI/reason fields are bounded
  arrays; shutdown has no completion pointer.
- `OperationTable` owns exactly 16 stable records. Each accepted record owns
  exactly one `VoipEventQueue::Reservation`, assigns nonzero monotonic IDs,
  wraps `UINT32_MAX` to 1, skips live collisions, and restores the ID cursor
  on admission rollback.
- `CommandMailbox` is a fixed 16-entry MPSC/single-consumer ring. `Admit()`
  serializes producers and performs validation, operation reservation,
  terminal-event reservation, value copy, and publication in that order.
- Rollback covers full operation/event/mailbox capacity. `AdmitDial()` copies
  and bounds a borrowed `DialRequest` synchronously.

## Verification

- `OperationMailboxTest PASSED` with strict warnings, no exceptions/RTTI,
  `-pthread`.
- Focused binary repeated 100 times.
- ASan/UBSan focused binary passed (`detect_leaks=0`; LeakSanitizer is not
  usable in this ptraced runner).
- Earlier Plan 2 tests passed: `AgentRegistryTest`, `HandlePoolTest`, and
  `VoipEventQueueTest`; `PublicContractTest.cpp` compiled with strict flags.
- `git diff --check` passed for all changed files.

## Notes

The registry-specific stale-handle check is supplied through the explicit
synchronous `CommandHandleValidator` interface at admission; structural
generation checks still reject zero-generation handles before the interface is
consulted.

## Fix Round 1

Review corrections removed the public record-pointer API and stateless
validator callback. `CommandHandleValidator` is now a synchronous per-instance
interface whose registry/pool bindings are never stored. Handle-bearing
commands require it; shutdown is the sole handleless command. Operation-table
rollback and completion are ID-based, and the only direct mailbox path is the
service-owned, operation-zero `TryPushShutdown()`.

Terminal reasons are always copied into the bounded event field, truncating at
`max_reason_length` for null, exact-bound, and oversized inputs. A failed
terminal commit retains the live record and reservation for retry or explicit
rollback. Tests cover real AgentRegistry generations, fake generation-aware
call validation, record reuse with delayed IDs, stop-order commit failure,
capacity isolation, and one-event completion after every recovery.

Fix-round verification: strict focused test, 100 repetitions, ASan/UBSan,
AgentRegistry/HandlePool/VoipEventQueue regressions, PublicContract compile,
and diff-check all passed.

## Fix Round 2

Operation records now have explicit `free`, `provisional`, and `accepted`
states. `Reserve()` creates a provisional record; `AcceptAdmission()` is the
mailbox commit point; only `RollbackAdmission()` can release a provisional
record, and accepted records can only finish through `Complete()`. This keeps
queued accepted IDs live and prevents rewind/reuse or stale completion after
record reuse.

Generic `Admit()` rejects `shutdown` before any reservation. The sole direct
shutdown path is `TryPushShutdown()`, which publishes operation-zero shutdown
once per mailbox lifecycle and rejects duplicates before and after pop.

Round-2 tests cover exact/max-plus-one/null reason handling, provisional and
accepted state transitions, accepted-operation exhaustion after draining the
mailbox, shutdown one-shot behavior, and terminal-event/capacity restoration.
Focused strict tests, 100 repetitions, ASan/UBSan, Plan 2 regressions, and
`git diff --check` passed.
