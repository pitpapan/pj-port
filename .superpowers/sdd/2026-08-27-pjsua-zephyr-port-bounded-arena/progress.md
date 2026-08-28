# SDD ledger — plan: docs/superpowers/plans/2026-08-27-pjsua-zephyr-port-bounded-arena.md

## Setup

- Worktree: `/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1`
- Branch: `codex/pjsua-port-plan1`
- Merge base: `fd70c933e`
- Baseline build: `sdk_contract.conf` compiled successfully for `mps2/an385`.
- Baseline run: printed `VOIP SDK CONTRACT RESULT: PASSED`; QEMU then idled and was terminated by the 30-second harness timeout.
- Build invocation must run `west` from `/home/pitpapan/zephyrproject` with the application source set to the absolute worktree path. The worktree's local west manifest cannot load the external `zephyr/` checkout. This uses only documented `west build`/`west build -t run` operations and does not inspect Zephyr source.
- Build environment: `PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin`, `CCACHE_DIR=/tmp/voip-plan1-ccache`, `CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp`.

## Preflight dependency and consistency scan

| Tasks | Produced/consumed file or interface | Finding / ruling |
|---|---|---|
| Task 1 internal | `PJSIP_INVITE_SOURCES`/`PJSIP_REGC_SOURCES` already enter the current `pjsip` target; Task 1 creates a distinct full `pjsip-ua` target containing the same sources | Conflict. Ruling: move ownership of INVITE/REGC objects to `pjsip-ua` when the new full UA boundary is enabled, while preserving existing minimal-profile behavior and tests. Never compile the same translation unit into both PJ libraries. Cost if wrong: older phase profiles may regress or duplicate symbols may remain. |
| Task 1 internal | The plan gives an audited initial source closure and also says rebuild until it links | Empirical boundary, not permission for broad inclusion. Ruling: Luna may add an explicit source only when a compiler/linker diagnostic proves it is required by unmodified PJSUA; every addition and diagnostic goes in the task report. No globs, PJSUA2, TLS, or SRTP. Cost if wrong: omitted link dependencies delay the task; excessive sources increase footprint. |
| Task 1 internal | Audio-device core is enabled while platform sound is disabled | Consistent: PJSUA links the audio-device API but runtime calls `pjsua_set_no_snd_dev()` and host backends remain disabled. |
| Task 1 → Task 2 | `pjproject/Kconfig`, `config_site.h`, `pjsua_link.c`, `pjsua_link.conf` | Consistent. Task 1 creates the boundary/probe; Task 2 freezes limits and adds assertions without changing lifecycle ownership. |
| Task 1 → Task 3 | PJ source manifest/CMake and PJSUA lifecycle consume PJ pool policy | Consistent. Task 3 adds one narrow allocator hook; it must not spread product allocation logic through PJSUA sources. |
| Task 1 → Task 4 | `pjsua_link.c` and `pjsua_link.conf` | Consistent, with callback-proof caveat ruled below. Task 4 extends rather than duplicates the probe. |
| Task 1 → Task 5 | PJSUA account/call APIs and application Kconfig/CMake | Consistent. Task 5 consumes the port; it does not alter port ownership. |
| Task 1 → Task 6 | Exact source closure and build/run markers | Consistent. Task 6 records measured evidence only. |
| Task 2 internal | Fixed Kconfig limits and runtime config values | Consistent: compile-time `5/7/12` and runtime `max_calls=7`, `max_media_ports=12` must agree before `pjsua_init()`. |
| Task 2 → Task 3 | `CONFIG_PJSUA_ARENA_BYTES` and `config_site.h` | Consistent. Task 2 defines the build-time capacity; Task 3 owns allocation behavior. |
| Task 2 → Task 4 | Assertions and PJSUA probe | Consistent. Task 4 must preserve the exact limits and security-disabled assertions. |
| Task 2 → Task 5 | Seven-call capacity | Ordering conflict in one RED instruction: Task 5 cannot naturally demonstrate the old four-call runtime limit after Task 2 is committed. Ruling: Task 2's failing compile-time assertions are the authoritative old-default RED evidence. Task 5 must still first fail because its capacity harness/behavior is absent, then prove seven records plus eighth-call 486 on the implemented profile; it must not temporarily weaken the fixed Kconfig limits. Cost if wrong: no separate runtime replay of the obsolete four-call configuration. |
| Task 2 → Task 6 | Fixed limits and arena size | Consistent. Documentation records configured and measured values, not estimates. |
| Task 3 internal | Pool failure callback is `void`; plan requires a normal `PJ_ENOMEM` path rather than abort | Ruling: the installed callback records failure and returns, allowing pool allocation to return `NULL`; callers must be exercised for graceful `PJ_ENOMEM`. Do not throw an uncaught PJ exception or fall back to `malloc()`. Cost if wrong: a PJ call site that assumes allocation success could still fail non-gracefully and must be exposed by exhaustion tests. |
| Task 3 → Task 4 | Arena install/reset/stats API | Consistent. Task 4 must install before `pjsua_create()` and prove post-destroy zero live blocks. |
| Task 3 → Task 5 | Arena baseline and native account/call records | Consistent. Task 5 compares cleanup to the post-start baseline, then complete destroy to zero. |
| Task 3 → Task 6 | Arena statistics | Consistent. Task 6 consumes measured capacity, peak, live-block, and exhaustion data. |
| Task 4 internal | Same-thread callback assertion can pass vacuously if the lifecycle produces no callback | Conflict with meaningful-test rubric. Ruling: require a nonzero actor-loop callback observation, using a PJSUA-scheduled timer callback if the bare lifecycle produces no registration/call callback, and separately guard every installed PJSUA callback-table trampoline with the actor thread ID. The report must not claim callback proof from a zero-count assertion. Cost if wrong: timer dispatch proves event-loop affinity but not every future network callback path; later account/call plans retain their own callback tests. |
| Task 4 → Task 5 | Actor-driven lifecycle and call-record harness | Consistent. Task 5 must continue using caller-driven event handling and no worker threads. |
| Task 4 → Task 6 | Thread/lifecycle evidence | Consistent. Task 6 records observed thread and stack evidence; it must distinguish QEMU/main-thread evidence from later production actor qualification. |
| Task 5 internal | Five accounts, seven held incoming records, eighth 486 | Consistent with PJSUA's locally verified `alloc_call_id()` behavior. The harness must make failures observable and clean all records. |
| Task 5 → Task 6 | Capacity pass marker and resource baseline | Consistent. Task 6 records commands/output without upgrading QEMU evidence to product acceptance. |
| Task 6 internal | Footprint and stack data are required but target thresholds belong to Plan 6 | Consistent. Record actual QEMU values; do not invent a target-board budget or claim production completion. |

## Rulings

- Ruling: distinct `pjsip-ua` ownership must avoid compiling INVITE/REGC sources into both `pjsip` and `pjsip-ua` — preserves the requested library boundary and existing minimal profiles — cost if wrong: regressions in phase profiles or duplicate symbols.
- Ruling: explicit linker-driven source additions are allowed only with diagnostics in the report — unmodified PJSUA has a broader closure than the initial manifest can prove statically — cost if wrong: link delays or excess footprint.
- Ruling: Task 5 uses Task 2's old-default compile assertion as obsolete-limit RED evidence and keeps fixed limits during its own runtime TDD — task ordering otherwise makes the historical runtime replay contradictory — cost if wrong: no separate runtime replay at four calls.
- Ruling: the arena OOM callback records and returns so PJ pool calls receive `NULL`; exhaustion tests must expose any unhandled caller — matches the public `PJ_ENOMEM` requirement without abort/fallback — cost if wrong: an unchecked PJ allocation path may still fail non-gracefully.
- Ruling: Task 4 requires at least one actor-loop callback observation and guards all PJSUA trampolines — avoids a vacuous same-thread test — cost if wrong: timer affinity alone does not prove future network callback paths, which later plans must verify.

## Task 1 completion ledger

- Status: complete on `codex/pjsua-port-plan1`; implementation and acceptance report are in this worktree.
- Link RED: an isolated pre-manifest copy built the probe object but failed at final link with undefined `pjsua_create`, `pjsua_config_default`, `pjsua_init`, `pjsua_start`, `pjsua_handle_events`, and `pjsua_destroy` (rc 1).
- Link GREEN: the explicit PJNATH, PJSIP SIMPLE, PJSIP-UA, PJSUA, PJMEDIA, audio-device, resolver, and crypto/XML closure built successfully (rc 0). `stun_simple_client.c` and `stun_simple.c` were retained after the linker specifically reported missing `pjstun_get_mapped_addr2` when omitted.
- Runtime: PJSUA lifecycle printed `PJSUA LINK RESULT: PASSED` and `No SIP worker threads created`; the bounded 30-second QEMU run exited 124 after the marker while idle.
- Regression: pristine `sdk_contract.conf` build exited 0 and its bounded run printed `VOIP SDK CONTRACT RESULT: PASSED`, then exited 124 on the idle-QEMU timeout.
- Scope note: Task 2 limits/assertions, Task 3 arena, and later lifecycle/capacity tests remain intentionally untouched.

## Task 1 review-fix ledger

- Status: review fixes complete on `codex/pjsua-port-plan1`; original commit
  `b8bb579f6` is preserved and the focused fix is committed separately.
- `pjsua_media.c` now selects `PJMEDIA_EVENT_MGR_NO_THREAD` only under
  `defined(PJ_ZEPHYR) && PJ_ZEPHYR!=0`; the Zephyr build/runtime lifecycle
  passed, while the report limits the evidence to code-path plus lifecycle
  proof pending Task 4's explicit PJMEDIA thread enumeration.
- `CONFIG_PJSUA` now depends on PJMEDIA SDP, SDP negotiation, endpoint, G.711,
  RTP/RTCP, UDP transport, stream, and audio-device gates. An incomplete
  profile first reproduced link RED, then was rejected during Kconfig/CMake
  generation after the fix.
- Moved `stun_simple_client.c` and `stun_simple.c` into the explicit
  `PJLIB_UTIL_PJSUA_SOURCES` family; minimal phase3/phase5 profiles built
  without those files and the full PJSUA profile built with them.
- Expanded regressions: phase3 account/regc build exited 0 and runtime printed
  its PASS marker before timeout 124; phase5 INVITE+REGC build exited 0 and
  runtime printed its lifecycle PASS marker and SIP 486 responses before its
  30-second timeout 124; SDK contract evidence remains above.
