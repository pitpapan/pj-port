# PJSUA-LIB Zephyr port acceptance record

This record captures the Plan 1 PJSUA-LIB C API port on `mps2/an385` at the
accepted Plan 1 head.  The application source is the absolute worktree path
shown in each command; `west` is run from the parent workspace because the
linked worktree does not own the west manifest.

## Port contract

The port links the C PJSUA-LIB API (not PJSUA2) with an explicit Zephyr source
manifest in `pjproject/zephyr/sources.cmake`.  The selected manifest families
are exactly:

| Boundary | Selected manifest families |
| --- | --- |
| PJLIB | `PJLIB_COMMON_SOURCES`, `PJLIB_PLATFORM_SOURCES` |
| PJLIB-UTIL | `PJLIB_UTIL_SOURCES`, `PJLIB_UTIL_DNS_RESOLVER_SOURCES`, `PJLIB_UTIL_PJSUA_SOURCES` |
| PJMEDIA | `PJMEDIA_SDP_SOURCES`, `PJMEDIA_SDP_NEG_SOURCES`, `PJMEDIA_ENDPOINT_SOURCES`, `PJMEDIA_G711_SOURCES`, `PJMEDIA_RTP_RTCP_SOURCES`, `PJMEDIA_LOOP_TRANSPORT_SOURCES`, `PJMEDIA_UDP_TRANSPORT_SOURCES`, `PJMEDIA_STREAM_SOURCES`, `PJMEDIA_PJSUA_SOURCES`, `PJMEDIA_PJSUA_RESAMPLE_SOURCES`, `PJMEDIA_PJSUA_CODEC_SOURCES`, `PJMEDIA_AUDIODEV_SOURCES` |
| PJSIP | `PJSIP_CORE_SOURCES`, plus `PJSIP_TCP_TRANSPORT_SOURCES` for the link profile; capacity also selects `PJSIP_UDP_TRANSPORT_SOURCES` and enables the INVITE gate |
| PJSUA closure | `PJNATH_SOURCES`, `PJSIP_SIMPLE_SOURCES`, `PJSIP_UA_SOURCES`, `PJSUA_SOURCES` |

These are explicit lists, not globs.  When PJSIP-UA is enabled, its
`PJSIP_UA_SOURCES` owns the INVITE/REGC objects; the standalone
`PJSIP_INVITE_SOURCES` and `PJSIP_REGC_SOURCES` families are not duplicated
into the PJSIP archive.  The PJSUA-gated PJLIB-UTIL family keeps
`stun_simple_client.c` and `stun_simple.c` in the link closure even though
STUN is disabled at runtime.  INVITE/REGC ownership remains in the selected
PJSIP-UA boundary when that boundary is enabled, avoiding duplicate objects.

The fixed embedded limits are five accounts, seven native PJSUA call records,
and twelve conference ports, with these exact Kconfig settings:

```text
CONFIG_PJSUA_MAX_ACCOUNTS=5
CONFIG_PJSUA_MAX_CALLS=7
CONFIG_PJSUA_MAX_CONF_PORTS=12
CONFIG_PJSUA_ARENA_BYTES=2097152
```

`config_site.h` maps them directly to the effective PJPROJECT macros
`PJSUA_MAX_ACC=CONFIG_PJSUA_MAX_ACCOUNTS`,
`PJSUA_MAX_CALLS=CONFIG_PJSUA_MAX_CALLS`, and
`PJSUA_MAX_CONF_PORTS=CONFIG_PJSUA_MAX_CONF_PORTS`.  Seven records are
required for the product topology's two promoted calls plus five queued
incoming calls.  The Plan 1 capacity harness holds seven incoming
records and verifies that the eighth receives SIP 486 Busy Here; scheduling and
promotion policy remain later-plan work.

All PJ pool blocks used by PJSUA are allocated from the single aligned static
arena owned by `pj_zephyr_pool_arena.c`.  Its callbacks are installed before
`pjsua_create()`, allocation has no general-heap fallback, and destroy/reset
returns the arena to zero live blocks and zero used bytes.  The 2 MiB arena is
distinct from the Zephyr system heap and the general libc malloc arena.

Compile-time disabled features are TLS (`PJSIP_HAS_TLS_TRANSPORT=0`), SRTP
(`PJMEDIA_HAS_SRTP=0`, SRTP transport and SDES gates off), video
(`PJMEDIA_HAS_VIDEO=0`), and host audio backends; the null audio device is the
only selected backend.  Link and arena profiles compile TCP signaling while
leaving the standalone `CONFIG_PJSIP_INVITE=n` and
`CONFIG_PJSIP_UDP_TRANSPORT=n` gates unset; their enabled PJSIP-UA family
still owns `sip_inv.c` and `sip_reg.c` without duplicate translation units.
The capacity profile enables its INVITE and UDP gates for the TCP INVITE
harness, with INVITE/REGC ownership still held by PJSIP-UA.  Runtime
configuration disables STUN, TURN, ICE, and UPnP, uses no sound device, and
sets PJSUA and PJMEDIA thread counts to zero.  PJSUA creates no worker thread;
the caller drives `pjsua_handle_events()`.

## Footprint and runtime evidence

Build output reports static image usage in the 4 MiB QEMU RAM/FLASH regions.
The map's `_image_ram_size` agrees with each RAM value.  The normal link map
also shows `.bss.arena = 0x200000` (2 MiB), `malloc_arena = 0x100000`,
`pjsua_var = 0x2c878`, `z_main_stack = 0xc000`, and
`kheap__system_heap = 0x4005c`; these are static image allocations, not PJ
arena usage.

| Profile | FLASH/ROM | RAM/static image | Configured PJ arena | Runtime arena evidence |
| --- | ---: | ---: | ---: | --- |
| `pjsua_link.conf` | 527260 B | 3734744 B (89.04%) | 2097152 B | Five lifecycle checks observed `0 < peak <= capacity`, then clean destroy; this harness does not print the numeric peak. |
| `pjsua_arena.conf` | 76280 B | 3527192 B (84.09%) | 2097152 B | Exhaustion/coalescing/100-cycle checks passed; the test checks peak internally but emits no numeric peak marker. |
| `pjsua_capacity.conf` | 551872 B | 3734968 B (89.05%) | 2097152 B | `destroyed used=0 live=0 peak=274528`; baseline/cleanup `used=90192 live=32` matched exactly. |

The non-numeric link/arena entries are intentional: Task 6 does not alter the
accepted test sources merely to add diagnostic printing, so no peak value is
invented.  The capacity profile's emitted peak is the numeric high-water
measurement available from the accepted harness.

Normal link runtime emitted five `callbacks=1 actor-affine` lifecycle lines,
`No SIP worker threads created` on each lifecycle, and:

```
PJSUA LINK RESULT: PASSED (5 lifecycles, arena clean)
```

The arena runtime emitted:

```
PJSUA ARENA RESULT: PASSED (exhaustion, coalescing, 100 cycles)
```

The capacity runtime emitted:

```
PJSUA CAPACITY CLEANUP: baseline used=90192 live=32 cleanup used=90192 live=32
PJSUA CAPACITY ARENA: destroyed used=0 live=0 peak=274528
PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)
```

Each bounded QEMU run returned 124 because `timeout` stopped the intentionally
idle emulator after the required marker.  This is expected and is not a test
failure.

## Stack diagnostic and qualification boundary

The following separate pristine link build enables Zephyr's documented
diagnostic Kconfig overrides without changing `pjsua_link.conf`:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DISABLE=1 \
  CCACHE_DIR=/tmp/voip-plan1-task6-diag-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task6-diag-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task6-diag -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf \
  -DCONFIG_THREAD_ANALYZER=y \
  -DCONFIG_THREAD_ANALYZER_USE_PRINTK=y \
  -DCONFIG_THREAD_ANALYZER_AUTO=y \
  -DCONFIG_THREAD_ANALYZER_AUTO_INTERVAL=5 \
  -DCONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=262144 \
  -DCONFIG_HEAP_MEM_POOL_SIZE=131072
```

The diagnostic build exited 0 and reported FLASH 528336 B and RAM 2818432 B
(67.20%).  The two reduced heap values are command-line-only diagnostic
overrides needed to keep analyzer overhead within the 4 MiB QEMU image; they
are not the production/link-profile footprint.  Its bounded run returned 124
after the same PJSUA pass marker and emitted six kernel-thread records plus
`ISR0`.  One record is the analyzer's own thread.  The map identifies the
caller thread record `0x2022f4c8` as `z_main_thread`; its largest observed
watermark was `usage 9240 / 49152 (18 %)` (unused 39912 bytes).

This is QEMU port evidence for the main thread that currently calls
`pjsua_handle_events()`, and therefore the future actor surrogate only.  Plan
1 has no production VoipRuntime actor thread; actor stack thresholds and target
qualification are explicitly deferred to Plan 6.  Analyzer output and its
extra thread are diagnostic overhead, not PJSUA worker threads.

The exact diagnostic run command was:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DISABLE=1 \
  CCACHE_DIR=/tmp/voip-plan1-task6-diag-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task6-diag-ccache-tmp \
  timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build \
  -d /tmp/voip-plan1-task6-diag -t run
```

## Reproduction commands

The exact pristine commands used for the accepted profiles were:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 \
  CCACHE_DIR=/tmp/voip-plan1-task6-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task6-link -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf

env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 \
  CCACHE_DIR=/tmp/voip-plan1-task6-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task6-arena -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_arena.conf

env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 \
  CCACHE_DIR=/tmp/voip-plan1-task6-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task6-capacity -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_capacity.conf
```

The exact acceptance run commands (all returned 124 after their PASS marker)
were:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DIR=/tmp/voip-plan1-task6-ccache CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task6-link -t run

env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DIR=/tmp/voip-plan1-task6-ccache CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task6-arena -t run

env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin CMAKE_BUILD_PARALLEL_LEVEL=4 CCACHE_DIR=/tmp/voip-plan1-task6-ccache CCACHE_TEMPDIR=/tmp/voip-plan1-task6-ccache-tmp timeout 40s /home/pitpapan/zephyrproject/.venv/bin/west build -d /tmp/voip-plan1-task6-capacity -t run
```

Acceptance is measured evidence only: Plan 6 owns production actor-stack
thresholds and target-board qualification.
