# PJSIP Port: PJLIB Baseline

This document freezes the PJLIB-only reference used before adding PJLIB-UTIL
or PJSIP to the Zephyr module. Later PJSIP phases should compare their resource
usage and runtime behavior against these numbers.

## Baseline identity

| Item | Validated value |
| --- | --- |
| Repository revision | `e32016c7c306cf6d89e81d8a99c08e351b699b95` |
| PJPROJECT/PJLIB | 2.16 |
| Zephyr | 4.4.0 |
| West | 1.5.0 |
| CMake | 4.4.2 from `.venv` |
| Zephyr SDK | 1.0.1 |
| ARM compiler | Zephyr SDK GCC 14.3.0 |
| Board | `mps2/an385` under QEMU |

All PJLIB port files required by the Zephyr module are tracked at this
revision. The generic PJPROJECT root CMake entry point remains available for
non-Zephyr builds, while Zephyr-specific build logic is contained in
`pjproject/zephyr/CMakeLists.txt`.

The current port profile explicitly assumes ARMv7-M little-endian targets in
`pjlib/include/pj/compat/os_zephyr.h`. This matches the Cortex-M3 QEMU baseline
and the intended Cortex-M7 product family, but it is not yet a portable Zephyr
architecture-selection mechanism.

## Clean-build resource baseline

| Validation | Flash | RAM | Configured heap | Dynamic threads | Thread stack |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stage 8 core | 99,836 B | 561 KB | 131,072 B | 48 | 8,192 B |
| Stage 9 network | 96,092 B | 115,744 B | 65,536 B | 5 | 8,192 B |
| Stage 10 ioqueue | 105,904 B | 344,736 B | 196,608 B | 8 | 8,192 B |

The high Stage 8 RAM figure is intentional: that test reserves 48 dynamic
thread slots with 8,192-byte stacks to exercise repeated thread lifecycles.

Stage 10 is the primary capacity baseline for PJSIP signaling:

| Resource | Effective limit |
| --- | ---: |
| `PJ_IOQUEUE_MAX_HANDLES` | 32 |
| `CONFIG_NET_MAX_CONTEXTS` | 40 |
| `CONFIG_NET_MAX_CONN` | 40 |
| `CONFIG_ZVFS_OPEN_MAX` | 48 |
| `CONFIG_ZVFS_POLL_MAX` | 40 |
| `CONFIG_MAX_PTHREAD_MUTEX_COUNT` | 48 |
| `CONFIG_DYNAMIC_THREAD_POOL_SIZE` | 8 |
| `CONFIG_DYNAMIC_THREAD_STACK_SIZE` | 8,192 B |

These are validation capacities, not automatically the final production
limits. PJSIP robustness testing must measure actual peak use before selecting
the product configuration.

## Validation commands

Each build was pristine and used one compiler job:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh

CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjlib_minimal \
  -d build-stage8 -- -DEXTRA_CONF_FILE=stage8.conf

CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjlib_minimal \
  -d build-stage9 -- -DEXTRA_CONF_FILE=stage9.conf

CCACHE_DISABLE=1 CMAKE_BUILD_PARALLEL_LEVEL=1 \
west build -p always -b mps2/an385 applications/pjlib_minimal \
  -d build-stage10 -- -DEXTRA_CONF_FILE=stage10.conf
```

Runtime validation used:

```sh
timeout 30s west build -d build-stage8 -t run
timeout 30s west build -d build-stage9 -t run
timeout 30s west build -d build-stage10 -t run
```

All three applications printed their unambiguous `PASSED` result. Each runner
then returned status 124 because `timeout` terminated QEMU after the test had
completed and Zephyr had entered its idle state. No QEMU ARM process remained.

## Known baseline diagnostics

The clean builds contain no compiler or linker errors. Existing warnings are:

- the QEMU icount configuration assignment is inactive;
- the test-only random generator is not suitable for production security;
- unused code in PJLIB `os_info.c` and `os_core_unix.c` for this configuration;
- a possible diagnostic-string truncation in PJLIB `unittest.c`;
- an empty entropy driver target in this QEMU configuration.

PJSIP digest authentication does not itself make the test random generator a
production entropy source. Any later security feature requiring cryptographic
randomness must add and validate a real entropy policy.

