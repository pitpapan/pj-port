# PJMEDIA Zephyr Port Execution Procedure

## 1. Purpose

This document is the execution playbook for
[`PJMEDIA_ZEPHYR_PORT_PLAN.md`](PJMEDIA_ZEPHYR_PORT_PLAN.md). The plan defines
what each phase is allowed to introduce and the claim it must prove. This
procedure defines how to implement, audit, test, document, and close that
phase.

Use the documents in this order:

1. [`PJMEDIA_PORT_ANALYSIS.md`](PJMEDIA_PORT_ANALYSIS.md) for architecture and
   known dependency boundaries;
2. [`PJMEDIA_ZEPHYR_PORT_PLAN.md`](PJMEDIA_ZEPHYR_PORT_PLAN.md) for phase scope
   and completion criteria;
3. this procedure for execution;
4. [`PJMEDIA_PHASE_VALIDATION_TEMPLATE.md`](PJMEDIA_PHASE_VALIDATION_TEMPLATE.md)
   for the evidence report.

Existing Phase 0 through Phase 3 validation reports remain historical records.
Use this procedure for all new phases and when rerunning an earlier phase.
Beginner-oriented Phase 4 through Phase 6 implementation guides are indexed in
[`pjmedia_steps/README.md`](pjmedia_steps/README.md).

## 2. Non-negotiable rules

- Do not inspect, search, index, or modify files under the workspace
  `zephyr/` directory. Zephyr is an external platform dependency.
- It is permitted to source `zephyr/zephyr-env.sh`, execute documented `west`
  commands, and read generated build output outside the Zephyr source tree.
- Do not start phase N+1 until phase N has a passing validation report.
- Add only sources required by the active phase. Never glob PJMEDIA sources.
- Keep Zephyr integration in `pjproject/zephyr/CMakeLists.txt`, module Kconfig,
  Zephyr-guarded configuration, and the validation application.
- Do not change the PJPROJECT root/native CMake path for a Zephyr-only need.
- Do not delete other operating-system sources from PJPROJECT. Exclusion from
  the Zephyr source list is sufficient.
- Do not add success stubs, dummy callbacks, weak replacements, or unrelated
  libraries merely to satisfy the linker.
- Do not hide unresolved archive references. Classify them and prove that all
  supported APIs resolve in the final ELF.
- Do not intentionally violate a `PJ_ASSERT_RETURN` precondition in a debug
  runtime test. Test malformed data at the parser/validator boundary and test
  valid state transitions through documented APIs.
- Treat compilation, public-API linkage, signaling, packet flow, decoded PCM,
  and physical audio as separate claims.
- Preserve existing user changes and keep generated build artifacts out of
  the source inventory.

If a phase requires reading a specific Zephyr implementation file, stop and
explain why documented interfaces and build diagnostics are insufficient.
Wait for explicit authorization before that inspection.

## 3. Phase entry gate

Before modifying files, complete this checklist:

- read `AGENTS.md`;
- read the active phase in the port plan;
- read the previous phase validation report;
- inspect current changes with `git status --short`;
- identify which existing changes belong to earlier phases;
- confirm the PJPROJECT version is still 2.16;
- confirm the tool environment and target board;
- confirm there is adequate filesystem space for the required pristine and
  link-probe builds;
- state explicitly that only the requested phase is starting.

Environment discovery commands:

```sh
pwd
sed -n '1,160p' AGENTS.md
git status --short
sed -n '1,20p' pjproject/version.mak

source .venv/bin/activate
source zephyr/zephyr-env.sh
west --version
west topdir
python --version
cmake --version
ninja --version
df -h .
```

Version output must be recorded in the validation report. A changed version
does not automatically fail the phase, but it invalidates direct comparison
with earlier measurements and requires a documented baseline rerun.

## 4. Phase scope record

Before editing, create a short phase scope record in the working notes or the
new validation document containing:

- phase number and exact goal;
- allowed source families;
- explicitly forbidden source families;
- production Kconfig symbol being activated;
- application validation selector;
- expected archive names;
- required public API surface;
- required runtime cases;
- previous-phase regressions to rerun;
- the exact statement that the phase will be allowed to claim.

Do not silently expand this scope after a compile or link failure. If the
actual closure differs, classify the new dependency first using Section 7.

## 5. File and naming convention

Use the following names unless the phase has a documented reason to differ:

```text
applications/pjmedia_minimal/phaseN_<feature>.conf
applications/pjmedia_minimal/src/phaseN_<feature>.c
applications/pjmedia_minimal/src/phaseN_link_probe.c
docs/PJMEDIA_PHASEN_VALIDATION.md
build-pjmedia-phaseN
build-pjmedia-phaseN-link-probe
build-pjmedia-phaseN-disabled
```

Production selectors belong in `pjproject/Kconfig`. Test-only selectors
belong in `applications/pjmedia_minimal/Kconfig`. Test-only sources must not be
added to the production library.

Each overlay must state important dependencies explicitly, including features
that must remain disabled. Do not rely on an unrelated board or application
default to preserve a phase boundary.

## 6. Standard phase workflow

Every buildable phase follows Steps 1 through 16 below. A documentation-only
phase may mark irrelevant steps not applicable, with a reason.

### Step 1: Freeze the pre-change boundary

Capture:

```sh
git status --short
git diff --check
git diff -- pjproject/Kconfig pjproject/zephyr/CMakeLists.txt \
  pjproject/pjlib/include/pj/config_site.h applications/pjmedia_minimal
```

Do not assume a dirty worktree is disposable. Earlier phase changes are part
of the active baseline unless the user explicitly requests otherwise.

### Step 2: Map candidate sources to the selected feature

Inspect only PJPROJECT headers, sources, and build metadata relevant to the
candidate family. Record:

- public functions the phase supports;
- direct PJ/PJLIB-UTIL/PJMEDIA/PJSIP calls made by each source;
- compile-time feature macros affecting those calls;
- static data and global initialization;
- optional sections that may carry unrelated dependencies;
- teardown functions and required ordering.

The candidate list is not validated merely because upstream places the files
in one library. Upstream desktop libraries often rely on other archive objects
or link targets not wanted in the embedded profile.

### Step 3: Decide whether the closure is acceptable

Classify each discovered dependency:

| Classification | Action |
| --- | --- |
| Already validated lower layer | Reuse it and cite its phase |
| Required real source in current phase | Add explicitly and document why |
| Required source assigned to a later phase | Stop; do not pull the later phase forward silently |
| Optional code removable by an existing supported macro | Set the macro explicitly in the Zephyr profile |
| Unrelated functions in a required source object | Keep visible, then prove supported APIs with a link probe |
| Missing platform implementation | Stop and design a real port or backend |
| Apparent upstream defect/refactor need | Present the exact proposed PJPROJECT change before editing |

Changing an upstream PJPROJECT production source requires a written reason,
platform guard where appropriate, native-behavior analysis, and a regression
for configurations where the new behavior is disabled.

### Step 4: Add or refine Kconfig gates

For every production symbol:

- define direct dependencies with `depends on`;
- keep the feature off by default unless the product profile explicitly owns
  the default;
- avoid `select` when it could bypass another feature's dependencies;
- describe what the symbol compiles and what remains excluded;
- keep board selection, device tree, and credentials out of generic Kconfig.

For every validation selector:

- depend on the exact production feature under test;
- exclude later features whose presence would invalidate the boundary;
- describe the runtime and audit claim;
- ensure `main.c` dispatches deterministically to one harness.

### Step 5: Activate an explicit CMake source group

Production CMake must:

- name each source explicitly;
- use the smallest production library that expresses the real ownership;
- add include paths only while their owning feature is enabled;
- use `zephyr_library_sources_ifdef()` or an equivalent explicit conditional;
- link only validated lower-layer targets;
- leave the root `pjproject/CMakeLists.txt` unchanged for Zephyr-only work.

After editing, review the diff before building:

```sh
git diff --check
git diff -- pjproject/Kconfig pjproject/zephyr/CMakeLists.txt \
  pjproject/pjlib/include/pj/config_site.h \
  applications/pjmedia_minimal
git diff --exit-code -- pjproject/CMakeLists.txt
```

### Step 6: Create the phase configuration overlay

The overlay must explicitly enable:

- PJLIB/PJLIB-UTIL prerequisites;
- only the production symbols under test;
- one application validation selector;
- required heap and stack instrumentation;
- documented networking options only for a network phase.

It must explicitly disable:

- later PJMEDIA source families;
- PJSIP or PJNATH unless the phase requires them;
- audio devices, video, SRTP, ICE, other codecs, and host facilities not in
  the active phase.

Begin with measured headroom, not an arbitrary production promise. Reduce
resources only after the tests pass and peaks are known.

### Step 7: Implement deterministic tests

The harness must:

- print an unambiguous phase start marker;
- register PJ error ranges needed for readable failures;
- report the test name, source line, numeric status, and error text;
- use fixed local addresses, payloads, samples, time inputs, and random seeds
  where possible;
- test success, documented rejection, configured boundary, and teardown;
- perform at least three full create/use/destroy lifecycles unless the plan
  specifies a stronger soak;
- check pool-factory checked-out counts and tracked PJ block counts after each
  teardown;
- record stack watermarks;
- print the final pass marker only after cleanup checks succeed.

Network and asynchronous tests must additionally:

- define callback ownership and quiescence;
- use bounded waits;
- count late callbacks after destruction;
- close transports before destroying their ioqueue or endpoint;
- avoid real external services in deterministic QEMU validation.

Real-time media tests must additionally record frame cadence, timestamp
increments, late periods, loss/reorder input, output counts, and hashes.

### Step 8: Run a pristine build

Use a new or deliberately pristine phase directory:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1

west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phaseN -- -DEXTRA_CONF_FILE=phaseN_<feature>.conf
```

Record:

- the exact command;
- configuration diagnostics;
- compile and link warnings;
- build step count;
- final flash and RAM;
- exit status.

Do not summarize a failed first attempt as a clean success. Record the failure,
root cause, correction, and exact continued or pristine rebuild command.

### Step 9: Audit the build graph and archive

Confirm the actual source list rather than trusting CMake intent:

```sh
rg -o "[^ ]*pjproject/pjmedia[^ ]*[.]c" \
  build-pjmedia-phaseN/build.ninja | sort -u

<sdk-arm-bin>/arm-zephyr-eabi-ar \
  t build-pjmedia-phaseN/modules/pjproject/libpjmedia.a

<sdk-arm-bin>/arm-zephyr-eabi-nm \
  -u build-pjmedia-phaseN/modules/pjproject/libpjmedia.a

<sdk-arm-bin>/arm-zephyr-eabi-nm \
  -g --defined-only build-pjmedia-phaseN/modules/pjproject/libpjmedia.a
```

Replace `<sdk-arm-bin>` with the actual SDK tool directory and record that
resolved command in the phase report.

Classify every undefined archive symbol as:

- libc/compiler runtime;
- validated PJLIB/PJLIB-UTIL/PJSIP dependency;
- call between selected PJMEDIA objects;
- optional unsupported function in a selected object;
- genuinely missing required implementation.

The last category is a phase failure until resolved with a real, approved
implementation. Optional unresolved sections require a whole-API probe when
they share an object with supported code.

### Step 10: Audit the final ELF

Run:

```sh
<sdk-arm-bin>/arm-zephyr-eabi-nm \
  -u build-pjmedia-phaseN/zephyr/zephyr.elf

<sdk-arm-bin>/arm-zephyr-eabi-nm \
  -g --defined-only build-pjmedia-phaseN/zephyr/zephyr.elf

rg --no-ignore -n -- "-ffunction-sections|-fdata-sections|--gc-sections" \
  build-pjmedia-phaseN/build.ninja
```

The final ELF must have zero unresolved required symbols. Create a
phase-specific forbidden source and symbol allowlist. A broad query such as
`pjmedia_` is insufficient once helper layers are legitimately present.

For each apparently forbidden match, identify the owning object and explain
whether it is:

- a supported helper API;
- an unused archive-only section;
- an accidentally linked later-phase implementation.

The third case fails the phase.

### Step 11: Run a whole-public-API link probe when required

A separate probe is mandatory when:

- a new public API family is introduced;
- normal tests exercise only part of a source object;
- a required object contains unrelated unresolved functions;
- section garbage collection could make an incomplete closure appear valid;
- the port plan explicitly requires it.

The probe must use a separate application selector, source, overlay, and build
directory. It must retain every supported public API under the normal build
flags. Verify the expected symbol names and count in the probe ELF.

Probe rules:

- use correct function-pointer types or unreachable volatile-guarded calls;
- do not form arguments by dereferencing null pointers, even in an unreachable
  branch, because the compiler may replace the path with a trap;
- do not disable garbage collection;
- do not use `--whole-archive` as proof of a clean supported closure beyond
  the normal Zephyr link policy;
- audit both the probe archive and final ELF;
- run the probe and require its explicit marker.

Build pattern:

```sh
west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phaseN-link-probe \
  -- -DEXTRA_CONF_FILE=phaseN_link_probe.conf
```

### Step 12: Run under bounded QEMU

Use a timeout appropriate to the phase:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh

timeout --signal=TERM --kill-after=5s 90s \
  west build -d build-pjmedia-phaseN -t run
```

Exit 124 is acceptable only when all of these are true:

- the final phase pass marker was printed;
- resource and teardown markers were printed before it;
- the application was idle after completion;
- timeout terminated QEMU rather than interrupting a running test;
- no QEMU process remains.

Check afterward:

```sh
ps -eo pid=,comm= | rg 'qemu-system-arm'
```

A crash, assertion, missing marker, hang during teardown, or timeout before the
final marker is a failure even if earlier cases passed.

### Step 13: Measure resources

At minimum, record:

- flash and static RAM from the linker report;
- configured Zephyr heap;
- PJ allocation blocks and bytes at peak;
- PJ pool used size at peak;
- checked-out pools and blocks after teardown;
- main and worker stack configured/used/unused values;
- delta from the previous phase.

Depending on the phase, also record:

| Phase family | Additional measurements |
| --- | --- |
| INVITE/signaling | dialogs, transactions, timers, transports, callbacks |
| RTP/transport | sockets, descriptors, network contexts, ioqueue handles |
| Stream | frames, packets, jitter buffer, cadence, CPU, lateness |
| Robustness | steady state, soak growth, exhaustion point, recovery |
| Product board/audio | DMA buffers, cache policy, latency, underrun/overrun, callback time |

An increased configured heap is not itself a failure, but unexplained growth
or missing teardown accounting is.

### Step 14: Run regressions

Every phase must perform:

1. the immediately previous phase's feature-disabled build-graph audit;
2. the immediately previous phase runtime, unless the current phase is purely
   documentation and changes no buildable code;
3. native/root CMake unchanged check for Zephyr-only work;
4. relevant PJSIP signaling regression when the new layer can affect signaling,
   sockets, timers, or shared ioqueue behavior.

Recommended commands:

```sh
west build -d build-pjmedia-phasePrevious
timeout --signal=TERM --kill-after=5s 90s \
  west build -d build-pjmedia-phasePrevious -t run

git diff --exit-code -- pjproject/CMakeLists.txt
git diff --check
```

If an older build directory no longer represents a pristine configuration,
recreate it rather than treating stale output as evidence.

### Step 15: Record evidence and clean disposable artifacts

Create `docs/PJMEDIA_PHASEN_VALIDATION.md` from the validation template.
Include successful and failed commands in chronological order.

Before deleting a disposable build directory, record:

- its final build result and footprint;
- exact source/archive audit;
- configuration gate audit;
- final ELF undefined-symbol audit;
- relevant runtime marker, if it was run.

Retain the main passing phase build until the user accepts the phase or disk
pressure requires removal. Link-probe and disabled-feature directories may be
removed after their evidence is captured. Delete only explicit verified build
paths, never a broad workspace path or unresolved variable.

### Step 16: Close the phase

The final check is:

```sh
git diff --check
git diff --exit-code -- pjproject/CMakeLists.txt
ps -eo pid=,comm= | rg 'qemu-system-arm'
git status --short
df -h .
```

Declare the phase passed only when every completion criterion in the plan and
every applicable procedure step has evidence. State explicitly that the next
phase was not started.

## 7. Failure and stop conditions

Stop the phase and report the exact blocker when any of these occurs:

- required closure reaches a source family assigned to a later phase;
- only a dummy implementation or stub would satisfy a required symbol;
- a PJPROJECT production refactor appears necessary and has not been approved;
- documented Zephyr interfaces and diagnostics are insufficient and Zephyr
  implementation inspection would be required;
- the feature-disabled configuration still links the new family;
- the all-public-API probe cannot link under normal garbage collection;
- the final ELF retains a forbidden later-phase implementation;
- malformed input causes corruption, uncontrolled assertion, or a hang;
- repeated lifecycle teardown leaves allocations, handles, timers, callbacks,
  threads, transports, or pools live;
- resource use exceeds the target or available build environment without a
  justified configuration change;
- the previous passing phase regresses.

Do not call a phase passed with a known completion item deferred. Mark it
blocked or incomplete and identify the smallest decision or implementation
needed to proceed.

## 8. Phase-specific execution gates

The port plan remains the complete test authority. The table below identifies
the additional procedure emphasis for each phase.

| Phase | Entry evidence | Mandatory execution emphasis | Exit claim |
| --- | --- | --- | --- |
| 0 | PJSIP Phase 11 report | pristine baseline, environment, resource reproduction, zero PJMEDIA objects | signaling baseline only |
| 1 | Phase 0 | default-off graph, explicit candidate groups, enabled/disabled builds, native CMake audit | configuration/header boundary |
| 2 | Phase 1 | exact three-source SDP archive, malformed/boundary inputs, clone/compare, teardown | SDP representation/parser/printer |
| 3 | Phase 2 | seven-source closure, archive undefined classification, 22-API probe, negotiation states | SDP offer/answer negotiation |
| 4 | Phase 3 and PJSIP baseline | exact INVITE/100rel/timer closure, module ordering, create/destroy only | INVITE support lifecycle |
| 5 | Phase 4 | loop transport, UAC/UAS state callbacks, CANCEL/BYE/timeout, no sockets/RTP | deterministic call control |
| 6 | Phase 5 and network baseline | UDP retransmission/error/peer-close behavior, shared signaling resources | UDP call control, no media |
| 7 | Phase 6 | endpoint with shared ioqueue/zero workers, direct G.711 registration, known vectors | media core and G.711 conversion |
| 8 | Phase 7 | packet bounds, wrap/loss/reorder, RTCP stats, jitter-buffer determinism | RTP/RTCP/jitter primitives |
| 9 | Phase 8 | loop transport before UDP, callback quiescence, socket/ioqueue exhaustion | media transport callbacks |
| 10 | Phase 9 | deterministic PCM source/sink, frame hash, cadence, loss/reorder, stream shutdown | headless G.711 stream |
| 11 | Phases 6 and 10 | real negotiated addresses/payloads, call-controlled media start/stop, ordered teardown | headless SIP media call |
| 12 | Phase 11 | soak, exhaustion, failure injection, unbounded-growth checks, all regressions | supported embedded limits |
| 13 | Phase 12 | same explicit profile on product MCU, LAN peer, entropy/cache/timing evidence | product-board headless media |
| 14 | Phase 13 | documented audio APIs, real factory/stream, bounded DMA ownership, hardware soak | product audio completion |

## 9. Teardown order reference

Each phase must adapt this order to the objects it owns. Destroy in reverse
dependency order and drain asynchronous work before freeing its storage:

1. stop generated media clocks and prevent new application work;
2. stop and destroy audio streams/devices;
3. stop and destroy PJMEDIA streams;
4. detach and destroy media transports and close RTP/RTCP sockets;
5. terminate INVITE sessions and dialogs;
6. drain PJSIP transactions, timers, and callbacks;
7. deinitialize codec factories;
8. destroy PJMEDIA endpoint/runtime objects;
9. unregister/destroy PJSIP modules and endpoint;
10. release phase pools;
11. destroy caching pools;
12. call `pj_shutdown()`;
13. verify zero live tracked resources.

Do not free a pool while callbacks, timers, ioqueue keys, transports, streams,
or negotiators still reference it.

## 10. Claim language

Use only the claim justified by the active exit gate:

- Phases 0–1: configuration or signaling baseline;
- Phase 2: SDP parsing/representation;
- Phase 3: SDP offer/answer negotiation;
- Phases 4–6: SIP call control without media;
- Phases 7–9: media primitives, not an audio stream;
- Phase 10: headless media stream, not a SIP-controlled or audible call;
- Phases 11–13: headless SIP media, not physical audio;
- Phase 14: product audio on the validated board/profile.

Do not describe PJMEDIA as fully ported merely because the current phase
passes. The initial product port is complete only at the Phase 14 product-audio
gate defined by the plan.
