# PJMEDIA Phase 0 Signaling Baseline

Date: 2026-08-20

## Result

PJMEDIA Phase 0 passed. The existing PJSIP Phase 7, Phase 10, and Phase 11
gates passed under QEMU before any PJMEDIA object was introduced. No
PJPROJECT source, PJMEDIA configuration, or application source was changed.

The workspace revision at validation time was:

```text
09eb9d4ee03b4cae8ed5e146ec7aa0f66fd5898d
```

PJPROJECT reports version 2.16. The active environment was:

| Component | Version |
| --- | --- |
| Zephyr | 4.4.0 |
| West | 1.5.0 |
| Python | 3.12.13 |
| CMake | 4.4.2 |
| Ninja | 1.10.1 |
| Zephyr SDK | 1.0.1 |
| ARM compiler | GCC 14.3.0 |
| Board | `mps2/an385` |

`docs/PJSIP_PHASE11_VALIDATION.md` remains representative of the active
configuration and measured result. Its image sizes and runtime resource
watermarks were reproduced exactly.

## Commands executed

Environment and revision discovery:

```sh
git rev-parse HEAD
git status --short
sed -n '1,12p' pjproject/version.mak

source .venv/bin/activate
source zephyr/zephyr-env.sh
west --version
west topdir
python --version
cmake --version
ninja --version
/home/pitpapan/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc --version
```

An initial build invocation was interrupted before configuration completed
when the request was repeated. Process and artifact checks confirmed that no
compiler or QEMU process remained and no `zephyr.elf` or `build.ninja` had
been produced. The pristine command was then executed:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1
west build -p always -b mps2/an385 applications/pjsip_minimal \
  -d build-pjsip-phase11 -- -DEXTRA_CONF_FILE=phase11_robustness.conf
```

The first uninterrupted attempt failed during CMake configuration in Zephyr's
Kconfig integration with an `if given arguments ... Unknown arguments
specified` diagnostic. No Zephyr implementation file was inspected. The same
pristine command was run again without changing source or configuration and
completed all 334 build steps successfully.

Runtime command:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
timeout --signal=TERM --kill-after=5s 120s \
  west build -d build-pjsip-phase11 -t run
```

The application completed all test gates, then remained idle. `timeout`
terminated QEMU at 120 seconds, so the wrapper returned status 124 after the
success markers. This is the expected bounded-run termination, not a test
failure.

Post-run evidence commands:

```sh
ps -eo pid=,comm= | rg 'qemu-system-arm'

rg -n "CONFIG_(HEAP_MEM_POOL_SIZE|MAIN_STACK_SIZE|DYNAMIC_THREAD_STACK_SIZE|DYNAMIC_THREAD_POOL_SIZE|NET_MAX_CONTEXTS|NET_MAX_CONN|ZVFS_OPEN_MAX|ZVFS_POLL_MAX|PJSIP_MAX_TRANSPORTS|PJSIP_MAX_TIMER_COUNT|PJSIP_MAX_PKT_LEN|PJLIB_IOQUEUE_MAX_HANDLES)=" \
  build-pjsip-phase11/zephyr/.config

rg -n "PJ_IOQUEUE_MAX_HANDLES" \
  pjproject/pjlib/include/pj/config_site.h \
  pjproject/pjlib/include/pj/config.h

rg -n "pjmedia/src|PJMEDIA" build-pjsip-phase11/build.ninja
```

The process query and PJMEDIA object query both returned status 1 with no
matches. Therefore no QEMU process remained and no PJMEDIA source was linked.

## Build baseline

The successful pristine build reported:

```text
FLASH: 255280 B / 4 MB (6.09%)
RAM:   602904 B / 4 MB (14.37%)
```

Non-fatal diagnostics were the existing QEMU icount/test-random-generator,
empty entropy-driver, assertion, and upstream PJPROJECT unused-label/function,
possible `snprintf` truncation, and SIP parser return warnings. No new
PJMEDIA warning was possible because no PJMEDIA object was present.

## Runtime baseline

Required markers:

```text
PHASE 7 RESULT: PASSED (2/2 lifecycles)
PHASE 10 RESULT: PASSED (2/2 lifecycles)
PHASE 11 RESULT: PASSED (5 complete lifecycles; 30-second active soak)
```

The active Phase 11 soak lasted 30,060 ms and completed 29 rounds and 232
OPTIONS requests. Teardown reported zero live PJ pools.

| Resource | Baseline |
| --- | --- |
| Zephyr heap | 393,216 B configured |
| PJ allocation | 290,384 B / 80 blocks peak |
| PJ steady allocation | 55,888 B / 11 pools |
| Main stack | 24,576 B configured; at most 13,744 B used |
| Event stack | 8,192 B configured; at most 2,472 B used |
| Transactions | 16 observed peak |
| Timers | 128 configured; 24 observed peak |
| PJSIP transports | 16 configured; 2 live during Phase 11 |
| Endpoint ioqueue | 16 handles tested; handle 17 returned `PJ_ETOOMANY` |
| PJLIB compile-time ioqueue ceiling | 64 handles |
| Network contexts/connections | 40 / 40 |
| Open/poll descriptors | 48 / 40 |
| SIP packet length | 4,000 B configured |

The PJMEDIA plan's starting-point prose currently mentions a shared ioqueue
maximum of 32 handles. The active Phase 11 image and
`docs/PJSIP_PHASE11_VALIDATION.md` both use and test an endpoint limit of 16;
this baseline records the measured value. Correcting plan prose is deferred
because Phase 0 forbids configuration work and Phase 1 was not started.
