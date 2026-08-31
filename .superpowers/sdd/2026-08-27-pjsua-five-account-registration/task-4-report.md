# Task 4 registration lifecycle report

## Current implementation record

- The callback router immediately copies native status, SIP code, bounded
  reason, renewal/unregistration flags, and expiration into a fixed record; the
  callback-router component test mutates both the source reason and callback
  fields after delivery and verifies the copy remains intact.
- The account manager maintains independent registration, refresh, retry, and
  native-account mappings for five agents. Retry jitter derives from
  `context.agent.slot`, never the scrambled native account ID.
- The fixed 32-record notification queue keeps one pending failure and one
  pending `retry_wait` notification per agent. With five agents that is at most
  ten guaranteed records, so a full queue always has coalescible progress
  data; the component test fills it and verifies the failure remains ordered
  before `retry_wait`.

## Failure diagnosis and regression evidence

The fresh QEMU run initially aborted immediately after
`PjsuaAccountManagerTest PASSED`. Temporary bounded checkpoints located the
first assertion in the exact-delay loop: native account ID 2 maps to agent slot
0 in the fake's deliberately scrambled map, but the test expected `2 * 50 ms`
jitter. The actual and correct due time was 1000 ms, not 1100 ms.

After correcting that assertion to use `context.agent.slot`, the next boundary
showed that each retry deadline correctly publishes `registering`; the test had
not consumed that notification before injecting the next failure. The test now
asserts that transition explicitly. A third boundary exposed retry state left
by the isolation fixture; the independent saturation case now clears all five
fixture retry states before advancing time to `UINT64_MAX`. All temporary
checkpoints were removed.

## Validation evidence

Fresh component execution from the main west workspace:

```sh
PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH \
CCACHE_DIR=/tmp/voip-plan3-ccache \
CCACHE_TEMPDIR=/tmp/voip-plan3-ccache-tmp \
timeout --signal=TERM --kill-after=2s 20s \
/home/pitpapan/zephyrproject/.venv/bin/west build \
-d /tmp/voip-plan3-pjsua-fake-task4 -t run
```

printed both required markers:

```text
PjsuaRegistrationStateTest PASSED
PJSUA PLAN 3 COMPONENT RESULT: PASSED
```

The QEMU invocation subsequently reports the known post-`main` Usage Fault
when the timeout terminates the emulator; it occurs after both required test
markers.

All ten host unit tests passed: `PublicContractTest`, `HandlePoolTest`,
`AgentRegistryTest`, `CallStateMachineTest`, `VoipEventQueueTest`,
`OperationMailboxTest`, `CallSchedulerTest`, `VoipServiceCoreTest`,
`NoHeapAfterInitTest`, and `ShutdownMailboxRegressionTest`.

## Task 4 audit

- Callback copy/mutation/truncation/null: covered by `PjsuaCallbackRouterTest`.
- 480; auth, permanent 4xx, transport/PJ, and recoverable 5xx/6xx mappings:
  covered by the table-driven registration-state test.
- Exact `1/2/4/8/16/30/30` retry schedule, slot jitter, deadline attempt
  advancement, refresh projection, immediate native failure, isolation, and
  saturated due-time arithmetic: covered by the registration-state test.
- The disabled-account check covers initial native registration only. A late
  failure callback for an already-disabled account can still schedule a retry;
  this is a remaining Task 4 concern.
- Notifications remain an account-manager seam and are not yet drained into
  `VoipRuntime`; connecting that seam is deferred to Task 5 and is outside this
  Task 4 repair.
