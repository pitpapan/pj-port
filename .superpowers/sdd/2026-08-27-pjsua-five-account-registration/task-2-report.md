# Task 2 report: PJSUA native runtime boundary

## Implementation summary

Added a narrow injectable `PjsuaApi` wrapper and actor-owned `PjsuaRuntime`
and `PjsuaTransportManager` components. The runtime installs/resets the Plan
1 arena, configures the fixed 5/7/12 PJSUA profile, disables SIP-message
logging, disconnects platform sound via `pjsua_set_no_snd_dev()`, and exposes
explicit event pumping. The transport manager supports precisely one plain
TCP transport and rejects TLS before native transport creation.

Added the process-global callback router trampoline with fixed copied
registration records, single active attachment, quiescence behavior, and the
temporary 486/hangup incoming-call guard. Added fake-backed QEMU component
tests and a component-test Kconfig/profile. Registry validation now rejects
PCM formats whose frame duration is not an integral millisecond value before
the PJSUA arena can be installed.

## Files changed

- `voip/src/pjsua/PjsuaApi.{hpp,cpp}`
- `voip/src/pjsua/PjsuaRuntime.{hpp,cpp}`
- `voip/src/pjsua/PjsuaTransportManager.{hpp,cpp}`
- `voip/src/pjsua/SignalingTransportPolicy.hpp`
- `voip/src/pjsua/PjsuaCallbackRouter.{hpp,cpp}`
- `voip/tests/pjsua/FakePjsuaApi.hpp`
- `voip/tests/pjsua/PjsuaRuntimeTest.cpp`
- `voip/tests/pjsua/PjsuaCallbackRouterTest.cpp`
- `voip/tests/pjsua/PjsuaPlan3TestMain.cpp`
- `voip/src/core/AgentRegistry.cpp`
- `voip/tests/unit/AgentRegistryTest.cpp`
- `applications/voip_integration/{CMakeLists.txt,Kconfig,pjsua_registration_fake.conf}`

## TDD evidence

The component tests/profile and references to the new `voip/src/pjsua/`
headers were added before the production components. The required RED build
was then attempted exactly as specified. This workstation cannot run the
build because its active west environment reports that CMake is not installed,
so it cannot reach the intentional missing-header compilation failure. This
is an environment blocker, not a passing RED result.

After implementation, host regression is GREEN (all eight host test binaries
reported `PASSED`). The QEMU GREEN marker could not be collected for the same
missing-CMake reason.

## Commands and results

1. `/home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 applications/voip_integration -d /tmp/voip-plan3-pjsua-fake -- -DEXTRA_CONF_FILE=pjsua_registration_fake.conf`
   - The worktree's west manifest lacks the build extension.
2. `/home/pitpapan/zephyrproject/.venv/bin/west -z /home/pitpapan/zephyrproject/zephyr build -p always -b mps2/an385 applications/voip_integration -d /tmp/voip-plan3-pjsua-fake -- -DEXTRA_CONF_FILE=pjsua_registration_fake.conf`
   - The supplied west instance also lacks the build extension.
3. From the main workspace, with the task worktree selected as the app source:
   `/home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration -d /tmp/voip-plan3-pjsua-fake -- -DEXTRA_CONF_FILE=pjsua_registration_fake.conf`
   - RED/Green QEMU execution blocked: `FATAL ERROR: CMake is not installed or cannot be found; cannot build.`
4. `./voip/tests/run_host_tests.sh`
   - PASS: PublicContract, HandlePool, AgentRegistry, CallStateMachine,
     VoipEventQueue, OperationMailbox, CallScheduler, and VoipServiceCore.
5. `git diff --check`
   - PASS (no output, exit 0).

## Self-review

- The native wrapper has compile-time capacity/profile checks and uses no
  PJSUA2, TLS, SRTP, accounts, media bridge, or codec policy.
- The runtime, transport manager, and router assert same-thread access in
  debug builds after their actor owner has been established.
- Fake tests exercise component-visible ordering and passed configuration,
  instead of asserting only fake calls.
- Concern: QEMU compilation/run has not been independently verified because
  the environment has no CMake. In particular, that remains necessary to
  validate final PJPROJECT/Zephyr C++ linkage.

## Validation Continuation

The venv CMake discovery issue was corrected by launching west from the main
workspace with `PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH`.

RED was reproduced in `/tmp/voip-plan3-task2-red`, detached at
`ed678f04d4a16f56886118084bddb0580356a6a6`, with only Task 2 test/profile
changes applied. The venv-prefixed build configured PJSUA successfully and
then failed at CMake generation as intended:

`Cannot find source file: ../../voip/src/pjsua/PjsuaApi.cpp`

GREEN first exposed the profile's missing C++17 selection and a misplaced
anonymous-namespace close in `PjsuaRuntime.cpp`. The corrective commit is
`7115311fd fix(voip): enable pjsua component test profile`; it selects
`CONFIG_STD_CPP17=y`, enables the network dependency PJSUA needs for its
media stream, and fixes the namespace definition.

The subsequent full GREEN cross-build was repeatedly interrupted by the
execution harness at approximately 30 seconds while compiling PJPROJECT.
Each retry restarted Ninja with `ninja: warning: premature end of file;
recovering`; it has not reached link or QEMU execution, therefore the required
`PJSUA PLAN 3 COMPONENT RESULT: PASSED` marker is not available yet.

The host suite was re-run after the fixes but was similarly interrupted by the
30-second execution limit after reporting these passes: PublicContract,
HandlePool, AgentRegistry, and CallStateMachine. A fresh full host-suite and
QEMU run remain required before claiming completion. `git diff --check` was
clean before the validation-fix commit.

## Link/profile and Task 2 audit continuation

The controller reproduced the intended profile's final-link failure and found
two direct causes: `pjsua_registration_fake.conf` inherited the Phase 1 entry
point from `prj.conf`, and it did not select the deterministic test random
generator used by the existing TCP PJSUA profiles. The profile also inherited
the QEMU SLIP serial backend, which prevented QEMU startup in this environment.

The profile now disables the Phase 1 and SLIP/TAP entry points, selects
`CONFIG_TEST_RANDOM_GENERATOR=y`, and enables PJPROJECT's UDP transport link
closure. The latter is required by the real `pjsua_transport_create()` symbol;
the component still creates exactly one `PJSIP_TRANSPORT_TCP` transport.

The strict Task 2 audit also corrected the callback router and its component
tests: the temporary incoming-call guard now uses injected `PjsuaApi` call
functions, tests prove its 486/hangup sequence and quiescent rejection while
registration callbacks continue to forward, and router entry points assert
actor affinity in debug builds. `Detach()` now has an explicit post-native-
destruction precondition. Failure-injection tests now assert the exact reverse
rollback sequence for every required failing stage; `PjsuaRuntime::Start()`
leaves the initialized runtime intact on failed start so the owner can close
the separately acquired transport before destroying PJSUA.

### Fresh commands and results

1. From `/home/pitpapan/zephyrproject`:
   `PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DIR=/tmp/voip-plan3-ccache CCACHE_TEMPDIR=/tmp/voip-plan3-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan3-pjsua-fake -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration -- -DEXTRA_CONF_FILE=pjsua_registration_fake.conf`
   - PASS: final link completed; image use was FLASH 542312 B and RAM 3692372 B.
2. From `/home/pitpapan/zephyrproject`:
   `timeout 30s env PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DIR=/tmp/voip-plan3-ccache CCACHE_TEMPDIR=/tmp/voip-plan3-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan3-pjsua-fake -t run`
   - PASS marker: `PJSUA PLAN 3 COMPONENT RESULT: PASSED`.
   - Exit 124 is expected: `timeout` stopped the intentionally idle QEMU
     target after the marker.
3. `voip/tests/run_host_tests.sh`
   - Could not obtain a clean full run during this continuation because the
     shared two-CPU environment was under sustained load. Attempts reached
     four or seven passing programs, then hit pre-existing timing-sensitive
     assertions in `VoipEventQueueTest` (100 ms producer wake-up) or
     `VoipServiceCoreTest` (startup coordination). No Plan 2 core/test source
     was changed; the controller will rerun this gate after contention clears.

## Review-fix continuation

Controller review found four correctness gaps and one profile-scope regression
in the Task 2 implementation. Focused tests were extended before the fixes:

- The callback-router fake now counts answer/hangup calls. The router test
  invokes incoming calls before and after `BeginQuiescence()` and requires a
  486 answer plus hangup for both while registration-state forwarding remains
  active.
- The router test calls `Detach()` before `MarkNativeDestroyed()` and proves
  the original active router still owns callbacks and rejects a second attach.
- The runtime test checks the normal transport `Id()` and repeats both
  `Shutdown()` and `Destroy()`.

The red component run aborted at the old debug-only `Detach()` assertion,
demonstrating that it did not provide a release-safe rejection. The minimal
fix keeps attachment and actor ownership unchanged when native destruction has
not been marked, adds the actor assertion to the out-of-line `Id()` entry
point, always invokes the temporary incoming-call guard, and scopes the app
include path/C++17 option to `CONFIG_VOIP_PJSUA_PLAN3_COMPONENT_TEST`.

### Review-fix validation

1. `PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DIR=/tmp/voip-plan3-ccache CCACHE_TEMPDIR=/tmp/voip-plan3-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan3-pjsua-fake -s /home/pitpapan/zephyrproject/.worktrees/pjsua-plan3/applications/voip_integration -- -DEXTRA_CONF_FILE=pjsua_registration_fake.conf`
   - PASS: component profile configured, compiled, and linked.
2. `timeout 30s env PATH=/home/pitpapan/zephyrproject/.venv/bin:$PATH CCACHE_DIR=/tmp/voip-plan3-ccache CCACHE_TEMPDIR=/tmp/voip-plan3-ccache-tmp /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan3-pjsua-fake -t run`
   - PASS marker: `PJSUA PLAN 3 COMPONENT RESULT: PASSED`.
   - Exit 124 is expected because timeout stopped the intentionally idle QEMU
     after the marker. The exact component QEMU process was confirmed absent
     afterward and its stale `qemu.pid` was removed only after confirming that
     it had no corresponding live process.
3. `git diff --check`
   - PASS (no output, exit 0).
4. The controller ran the full host suite after terminating the stale QEMU;
   all 10/10 host test binaries passed. This worker did not rerun it, per the
   controller instruction, to avoid reintroducing shared-host contention.
