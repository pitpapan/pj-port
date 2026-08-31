# Task 3 Report: Transactional PJSUA Account Manager

## Scope delivered

- Added exactly five stable `PjsuaAccountContext` slots and a fixed native-ID
  lookup. Native IDs are validated, so configuration indexes are never used as
  PJSUA account IDs.
- Added only the PJSUA account functions required by this component to the
  injectable API and its fake.
- Built each account from `pjsua_acc_config_default()`, attached stable user
  data before `pjsua_acc_add()`, borrowed registry-owned strings only while
  filling the native configuration, and bound the shared TCP transport.
- Disabled immediate registration, native retry, STUN, UPnP, contact/Via NAT
  rewriting, RFC5626, SRTP, and custom ICE/TURN selection.
- Added reverse-order transactional rollback, including user-data clearing,
  for add failure, duplicate native ID, and invalid native ID.
- Exposed bounded `Resolve()` and `NativeId()` prerequisites. Initial
  registration is a separate post-add method and only selects contexts marked
  `register_on_start`.

## Explicitly deferred

No registration callbacks/retry lifecycle (Task 4), runtime-adapter
composition/shutdown orchestration (Task 5), call/media, codecs, TLS, or SRTP
behavior was added.

## TDD and verification evidence

- RED: the specified component build failed with
  `fatal error: ../../src/pjsua/PjsuaAccountManager.hpp: No such file or directory`.
- GREEN: the same component profile built successfully after implementation.
- QEMU emitted `PjsuaAccountManagerTest PASSED` and
  `PJSUA PLAN 3 COMPONENT RESULT: PASSED`. The existing harness subsequently
  faults after `main()` returns; the bounded QEMU process was explicitly
  terminated and its stale `qemu.pid` removed.
- `voip/tests/run_host_tests.sh` completed successfully (all 10 host-test
  commands).
- `git diff --check` completed successfully.

## Review checklist

All Task 3 requirements are covered by fake-API tests: one/five accounts,
scrambled IDs, sixth/reinitialization rejection, duplicate and invalid IDs,
third-add rollback, exact copied account configuration, disabled native
features, disabled-agent registration, bounded lookup, and context ownership
shape. The manager uses only fixed arrays and no dynamic allocation.
