# PJMEDIA Phase N Validation

Date: YYYY-MM-DD

## Result

Status: `PASSED`, `FAILED`, or `BLOCKED`

State the exact phase claim in one paragraph. State explicitly whether Phase
N+1 was started. Do not use a broader PJMEDIA claim than the port plan permits.

## Scope

| Item | Phase N value |
| --- | --- |
| Goal | `<exact plan goal>` |
| Production symbol | `CONFIG_<symbol>` |
| Validation selector | `CONFIG_<test symbol>` |
| Board | `<board>` |
| Main build | `build-pjmedia-phaseN` |
| Link-probe build | `<path or not required>` |
| Previous regression | `<phase/build>` |
| Explicitly deferred | `<later families>` |

### Entry criteria

- [ ] Previous phase passed.
- [ ] Previous validation report was reviewed.
- [ ] PJPROJECT version was confirmed.
- [ ] Existing worktree changes were inventoried and preserved.
- [ ] Active phase and stop boundary were announced.

## Environment

| Component | Actual value |
| --- | --- |
| Workspace revision | `<git revision>` |
| PJPROJECT | `<version>` |
| Zephyr | `<version from documented build output>` |
| West | `<version>` |
| Python | `<version>` |
| CMake | `<version>` |
| Ninja | `<version>` |
| Zephyr SDK | `<version>` |
| Compiler | `<version>` |
| Board | `<board>` |
| libc / C++ runtime | `<configuration>` |

Record the exact discovery commands:

```sh
<commands in execution order>
```

## Production changes

List every production file changed and why:

| File | Change | Native/non-Zephyr effect |
| --- | --- | --- |
| `<path>` | `<reason>` | `<none, guarded, or explained>` |

List every validation-only file changed or added separately.

Confirm:

- [ ] No Zephyr source file was inspected or modified.
- [ ] PJPROJECT root/native CMake is unchanged, or an approved generic change
      is fully described.
- [ ] No unsupported OS source was deleted.
- [ ] No stub or dummy success implementation was added.

## Configuration boundary

Record relevant enabled and disabled symbols exactly as generated:

```text
CONFIG_<required>=y
# CONFIG_<forbidden> is not set
```

Explain every changed PJ/PJMEDIA/PJSIP compile-time macro and why it belongs to
this phase.

## Exact source and object closure

Expected sources:

```text
<one source per line>
```

Actual archive members:

```text
<arm-zephyr-eabi-ar t output>
```

Commands:

```sh
rg -o "[^ ]*pjproject/pjmedia[^ ]*[.]c" \
  build-pjmedia-phaseN/build.ninja | sort -u

<resolved-arm-ar> t \
  build-pjmedia-phaseN/modules/pjproject/libpjmedia.a
```

Explain any difference between expected and actual lists. An unexplained
difference means the phase has not passed.

## Build commands and results

Record every build attempt chronologically, including failures.

### Attempt 1

```sh
<exact command>
```

Result:

```text
<exit status and decisive output>
```

Root cause and correction:

`<explanation, or not applicable>`

Repeat subsections for all additional attempts. Identify whether the final
passing build was pristine or incremental.

Final footprint:

```text
FLASH: <bytes> / <capacity> (<percent>)
RAM:   <bytes> / <capacity> (<percent>)
```

Record all new warnings. Existing warnings may be summarized only after
confirming they were already present in the previous phase.

## Undefined-symbol audit

Commands:

```sh
<resolved-arm-nm> -u \
  build-pjmedia-phaseN/modules/pjproject/libpjmedia.a

<resolved-arm-nm> -u \
  build-pjmedia-phaseN/zephyr/zephyr.elf
```

Classify archive undefined symbols:

| Class | Symbols or families | Provider/disposition |
| --- | --- | --- |
| libc/compiler | `<list>` | `<provider>` |
| PJLIB/PJLIB-UTIL/PJSIP | `<list>` | `<validated phase>` |
| Selected PJMEDIA objects | `<list>` | `<object>` |
| Unrelated discardable sections | `<list>` | `<link-probe evidence>` |
| Missing required implementation | `<must be empty to pass>` | `<action>` |

Final ELF undefined-symbol result: `<zero, or failure details>`.

## Whole-public-API link probe

Required: `yes` or `no`, with reason.

If required, record:

- public header and supported API list;
- expected symbol count and exact names;
- independent selector, overlay, source, and build directory;
- proof that `-ffunction-sections`, `-fdata-sections`, and
  `--gc-sections` remained enabled;
- exact build and runtime commands;
- archive and final ELF audit;
- actual retained symbol list;
- final probe marker and footprint.

```sh
<exact commands>
```

```text
<results>
```

## Runtime tests

| Case | Expected | Actual | Result |
| --- | --- | --- | --- |
| `<success case>` | `<status/state/data>` | `<observed>` | PASS/FAIL |
| `<rejection case>` | `<controlled error>` | `<observed>` | PASS/FAIL |
| `<boundary case>` | `<limit behavior>` | `<observed>` | PASS/FAIL |
| `<teardown case>` | `<zero live resources>` | `<observed>` | PASS/FAIL |

Runtime command:

```sh
<exact command>
```

Final markers:

```text
<markers copied exactly>
```

Runner exit status: `<status>`. If it was 124, confirm the final marker and
teardown evidence occurred before timeout stopped idle QEMU.

## Lifecycle and teardown

State the number of complete lifecycles. Record actual destruction order and
the checks made after each lifecycle.

| Resource after teardown | Expected | Actual |
| --- | ---: | ---: |
| PJ allocated blocks | 0 | `<value>` |
| PJ allocated bytes | 0 | `<value>` |
| Checked-out pools | 0 | `<value>` |
| Threads | `<baseline>` | `<value>` |
| Timers | `<baseline>` | `<value>` |
| Sockets/ioqueue keys | `<baseline>` | `<value>` |
| Late callbacks | 0 | `<value>` |

Remove non-applicable rows only with a short reason.

## Resource measurements

| Resource | Previous phase | Phase N | Delta |
| --- | ---: | ---: | ---: |
| Flash | `<value>` | `<value>` | `<value>` |
| Static RAM | `<value>` | `<value>` | `<value>` |
| Zephyr heap configured | `<value>` | `<value>` | `<value>` |
| PJ allocated bytes peak | `<value>` | `<value>` | `<value>` |
| PJ pool used peak | `<value>` | `<value>` | `<value>` |
| Main stack used | `<value>` | `<value>` | `<value>` |

Add phase-specific socket, timer, dialog, packet, jitter, cadence, CPU,
latency, DMA, or audio measurements here.

## Forbidden-source and forbidden-symbol audit

Define the phase-specific forbidden set, then record exact commands and
results:

```sh
<build.ninja source queries>
<archive member queries>
<final ELF symbol queries>
```

No-match commands returning status 1 are successful only when the intended
meaning is explicitly stated.

## Regression results

### Previous PJMEDIA phase

```sh
<build and runtime commands>
```

Result: `<markers, archive boundary, and exit status>`.

### Feature-disabled build

```sh
<pristine command and audit>
```

Result: `<new objects absent/present>`.

### PJSIP/native regression

```sh
<commands, or reason not applicable>
```

Result: `<actual result>`.

## Failures and corrections

Record each unexpected event, even when corrected:

| Attempt | Symptom | Root cause | Correction | Scope impact |
| --- | --- | --- | --- | --- |
| 1 | `<failure>` | `<cause>` | `<change>` | `<none or explained>` |

Do not omit assertion failures, timeouts before the marker, compiler-elided
probe calls, more-specific status codes, filesystem failures, or stale build
configuration.

## Cleanup and retained artifacts

| Artifact | Action | Evidence captured first |
| --- | --- | --- |
| `build-pjmedia-phaseN` | retained/removed | `<evidence>` |
| `build-pjmedia-phaseN-link-probe` | retained/removed | `<evidence>` |
| `build-pjmedia-phaseN-disabled` | retained/removed | `<evidence>` |

Confirm no QEMU process remains.

## Final checks

```sh
git diff --check
git diff --exit-code -- pjproject/CMakeLists.txt
ps -eo pid=,comm= | rg 'qemu-system-arm'
git status --short
df -h .
```

- [ ] Every plan completion criterion has evidence.
- [ ] Every applicable procedure step passed.
- [ ] Exact source list is recorded.
- [ ] Archive undefined symbols are classified.
- [ ] Final ELF has no missing required symbol.
- [ ] Supported public APIs pass the required link probe.
- [ ] Runtime cases and repeated teardown pass.
- [ ] Previous phase and disabled-feature regressions pass.
- [ ] Resource peaks and deltas are recorded.
- [ ] No background QEMU process remains.
- [ ] Phase N+1 was not started.

## Conclusion

State one of:

- `Phase N PASSED: <narrow validated claim>.`
- `Phase N FAILED: <failed criterion>.`
- `Phase N BLOCKED: <exact missing decision or implementation>.`

List remaining limitations without converting them into an unsupported pass.
