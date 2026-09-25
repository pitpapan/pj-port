# Plan 4 PJSUA call-management promotion acceptance

Accepted source revision: `6238fe3749e79f1678320d51c9668cdd57215d82`. This record is for host tests and MPS2/AN385 QEMU behavior plus an RT1064 compile and memory report. It is not RT1064 hardware, external-network, audio-device, or production memory-map acceptance. Controller-managed independent final whole-branch review is pending at this acceptance-document commit.

The implementation retains one `VoipService`, actor, PJSUA runtime, callback router, core `CallStateMachine`, and global `CallScheduler`. Native PJSUA context/lease management supports incoming admission, outgoing promotion, controls, and copied terminal notifications. The scheduler owns the two-promoted-call limit and five-entry FIFO; queued outgoing calls do not create native INVITEs, and queued incoming calls retain native state while admission/rejection remains visible. The host, fake, and SIP tests exercise head blocking, ordered promotion, timeout origins, exactly-once operations, immediate handle invalidation with pollable copied terminal state, and five complete lifecycles.

## Final-head commands and verdicts

Run from `/home/pitpapan/zephyrproject/.worktrees/pjsua-plan4`:

```sh
bash voip/tests/run_host_tests.sh > /tmp/voip-plan4-final3-host-1.log 2>&1
bash voip/tests/run_host_tests.sh > /tmp/voip-plan4-final3-host-2.log 2>&1
```

Both exited 0. Each log has all eleven Plan 1–4 host PASSED markers: PublicContract, HandlePool, AgentRegistry, CallStateMachine, VoipEventQueue, OperationMailbox, CallScheduler, VoipServiceCore, NoHeapAfterInit, ShutdownMailboxRegression, and PjsuaCapacityDeclineDrain. Both logs have SHA-256 `44cf37f85e91e04653068fc19c36fb1ece43ecac139ba2b90547bb273b62e355`.

For each QEMU build/run below, run from `/home/pitpapan/zephyrproject` with `PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DISABLE=1 CCACHE_TEMPDIR=/tmp/voip-ccache` prefixed to each `west` or harness command. These are the exact command forms instantiated with the name/profile/timeout values in the table:

```sh
west build -p always -b mps2/an385 -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/applications/voip_integration -d /tmp/voip-plan4-final3-<name> -- -DEXTRA_CONF_FILE=<profile> > /tmp/voip-plan4-final3-<name>-build.log 2>&1
timeout <seconds>s west build -d /tmp/voip-plan4-final3-<name> -t run > /tmp/voip-plan4-final3-<name>-run.log 2>&1
```

The fake profile used the existing verdict harness instead of the second command: `bash /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/voip/tests/pjsua/run_plan3_component_qemu_regression.sh /tmp/voip-plan4-final3-fake /tmp/voip-plan4-final3-fake-run.log`. It exited 0. All builds exited 0; the four direct QEMU runs exited 124 only because their wrappers timed out after final PASS and clean idle. No run log contains a fault/fatal signature, including after its final marker. The scan covered MPU/Usage/Bus/Hard/MemManage faults, instruction/data access violations, invalid EXC_RETURN, Zephyr fatal error, assertion failure, kernel panic/oops, halting, and `abort()`; the fake harness also enforces the whole-log check.

| Name / profile / timeout | Build; FLASH / RAM bytes | Required QEMU evidence | Build / run SHA-256 |
| --- | --- | --- | --- |
| `fake` / `pjsua_registration_fake.conf` / harness | 446/446; 595,744 / 4,144,212 | `PjsuaPublicServiceTest PASSED`, `PjsuaRuntimeAdapterTest PASSED`, `PJSUA PLAN 3 COMPONENT RESULT: PASSED` | `c161afd613ec55b057133313768af69a16222780fa51f87bf26c394a0d6a1b07` / `3ea8bcb32817c93fa13e6ac298309f1e7a588756f137dff494260e85a0161c43` |
| `registration` / `pjsua_registration.conf` / 55 | 446/446; 586,212 / 3,847,432 | One-account TCP Digest/actor smoke, lifecycle 1–5 PASS, `VOIP PJSUA GENERAL HEAP: PASSED (0 allocations after initialization)`, `VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)` | `01618b2e810b3cd14d9cf452f8591f35e85735d8f5283a3ea8895a1ad2d7f93f` / `21bd4745355ef7dfd2bbbef1cdd68a869542430a53c773fc660bc185c3d15af5` |
| `capacity` / `pjsua_capacity.conf` / 55 | 430/430; 552,492 / 3,734,984 | `PJSUA CAPACITY CLEANUP: baseline used=90192 live=32 cleanup used=90192 live=32`; destroyed `used=0 live=0 peak=274528`; `PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)` | `e5f28e37ee7b7c531f87b1c9df38c5f93cceddf7b403e02dab9a3f1fdbf3cad1` / `2553296bf1f8ce2673751c12db155e6ace03f105a15932ba00a0c9b30a0a5b11` |
| `call-component` / `pjsua_call_component.conf` / 20 | 436/436; 90,116 / 1,421,872 | CallApi, CallContext, IncomingAdmission, OutgoingPromotion, and CallState individual PASSED; `PJSUA PLAN 4 COMPONENT RESULT: PASSED` | `26f350214d80672150b42e0a5f7a6053f37d44e17d96b126d375be0d901a3ccb` / `7a8766c9f15984d29d88d3d314489c11659217043583f4997158b539c2ce4a1a` |
| `multi-call` / `multi_call_scheduler.conf` / 70 | 446/446; 633,696 / 3,924,596 | Scripted TCP SIP peer, two promoted and five FIFO, ordered head blocking, five complete lifecycles; `VOIP MULTI CALL RESULT: PASSED (2 promoted, 5 FIFO, 5 lifecycles)` | `68f441552b6c88c91168bee5b958a57d7aeb5d3d42805bc52acc959ff693f27e` / `9ae68b3d7521649480108d028f623ccc0ae409881b3004d93c1e107a4ce8192f` |

The Plan 4 component also executes `RunPjsuaCallControlTests()` unconditionally; it does not print an individual marker, and its entry was confirmed in the focused CPU trace. The final-head whole-log scan found no later fault. The two prior QEMU post-PASS faults show why the full-log verdict is part of this acceptance, rather than treating a marker alone as success.

## Memory, stack, and teardown

The five final-head multi-call lifetime arena peaks are 467,568, 600,800, 518,848, 502,168, and 508,496 bytes against fixed 2,097,152-byte capacity: maximum 28.65%, below both the normal 75% and injected 90% ceilings. Each lifecycle then reports `arena peak=0 ... used=0 live=0 failed=0`, resets 1–5, `general_heap_allocations=0`, and no SIP worker threads. Peer minimum unused stack is 3,968 bytes. The real registration profile separately reports zero general-heap allocations over all five lifecycles; the capacity profile returns its private arena to zero after destroy. Terminal traces and teardown precede the final multi-call marker, leaving no logical/native call, operation, reservation, account, transport, router, credential, or arena ownership.

The fixed PJSUA actor stack is 32 KiB. The reviewed Plan 3 real-registration debugger probe measured 20,840, 20,840, 20,840, 20,840, and 20,808 bytes minimum unused across its five lifecycles. The focused Plan 3 fake CPU trace measured 19,360 bytes main-stack headroom after the stack repair. The focused Plan 4 call-component CPU trace measured a minimum main-thread SP 3,144 bytes above its unchanged 65,536-byte main-stack base, with zero below-base or idle-range samples before the expected idle switch. These are retained focused/probe measurements, not newly instrumented final-head QEMU runs; the final-head runs independently passed their whole-log fatal scans. Fake QEMU RAM use is 98.81% of its 4 MiB region, so these test fixtures are not a target RAM budget.

## RT1064 compile-only boundary

From `/home/pitpapan/zephyrproject`, with the same west environment prefix:

```sh
west build -p always -b mimxrt1064_evk -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan4/applications/voip_integration -d /tmp/voip-plan4-rt1064 -- -DEXTRA_CONF_FILE=pjsua_call_component.conf > /tmp/voip-plan4-final3-rt1064-build.log 2>&1
west build -d /tmp/voip-plan4-rt1064 -t ram_report > /tmp/voip-plan4-final3-rt1064-ram.log 2>&1
west build -d /tmp/voip-plan4-rt1064 -t rom_report > /tmp/voip-plan4-final3-rt1064-rom.log 2>&1
```

All exited 0. Build 463/463 linked with FLASH 113,028 / 4,194,304 bytes, RAM 1,430,780 / 33,554,432 bytes, and DTCM 13,936 / 131,072 bytes; ITCM, OCRAM, and OCRAM2 reported zero. The independent report roots are RAM 1,444,671 and ROM 113,016 bytes, which have different accounting from linker regions. Build/ram/rom log SHA-256: `22d6021e9395b7314fd530cd591e03bd82f9afb4337c1998ba30b44e9213fea0`, `1f0a4c33ff7f18c8e7d4ccd953eba2d0235cbafa406f9e55ece263c04a86bc19`, `98b9368d0cd4960451e30cf42d464c12f1c64c766841fc18b4d106e19c94e2ee`. No network driver was selected for product deployment, board image was run, or embedded networking/audio behavior qualified. The large RAM region and QEMU stress configuration must not be mistaken for a validated production memory map.

## Scope, repairs, and open review

The branch diff from Plan 3 base `28e8e43f39978c7e63b909da8ce41476793be489` spans 43 own-project paths, with no deletion or top-level `zephyr/` source change; `git diff --check` passes. The added Plan 4 path contains no media bridge/device audio, codec-priority policy, PJSUA2, TLS/SRTP enablement, runtime agent mutation, scheduler bypass, or second public business-state table. PJSUA context records are bounded native ownership; public call state remains in the core state machine/scheduler. The legacy uppercase VoIP stack remains intact. Profiles explicitly disable SIP TLS/SRTP; the separate legacy SRTP path is not promoted by this work.

Three independently scoped repairs preceded the final-head restart: `da5ec4c9d` fixed Plan 3 fake test stack lifetime, deterministic failure assertions, and whole-log fault verdict; `a4e567cf4` linked the call manager exactly once in the production VoIP library; `6238fe374` bounded the incoming-admission test frame and measured 3,144-byte main-stack headroom. Their REDs, scoped GREENs, disassembly, and traces are retained in `.superpowers/sdd/2026-08-27-pjsua-call-management-promotion/task-7C-report.md`. A deferred Minor remains: `multi_call_scheduler.conf` enables PJSIP UDP although its scripted SIP scenario uses TCP. It was not silently changed. Independent final whole-branch review is controller-managed and pending at this document commit; findings may require another focused repair and affected-gate restart.
