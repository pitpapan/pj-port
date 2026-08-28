# Task 2 report — freeze embedded PJSUA limits and security policy

## Summary

Task 2 freezes the embedded PJSUA compile-time record capacities at five
accounts, seven calls, and twelve conference ports. The future pool-arena
capacity is exposed as `CONFIG_PJSUA_ARENA_BYTES` with a 64 KiB–4 MiB range and
2 MiB default. The PJSUA link probe now derives runtime call/media limits from
the compile-time macros and rejects a mismatch before `pjsua_init()`.

TLS and SRTP remain explicit opt-outs. SIP TCP and plain RTP/RTCP UDP remain
the selected profile; no TLS/SRTP implementation or arena allocator was added.

## Exact files changed

- `pjproject/Kconfig`
- `pjproject/pjlib/include/pj/config_site.h`
- `applications/voip_integration/pjsua_link.conf`
- `applications/voip_integration/src/pjsua_link.c`
- `.superpowers/sdd/2026-08-27-pjsua-zephyr-port-bounded-arena/progress.md`
- This report

No PJPROJECT source manifests or source ownership changed.

## RED assertions and diagnostics

The five required `_Static_assert`s were added before changing Kconfig or
config-site mappings. The first build used the old upstream PJSUA defaults:

```text
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-task2-red-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task2-red-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task2-red -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
```

It exited 1 at `pjsua_link.c` with:

```text
error: static assertion failed: "PJSUA_MAX_ACC must be five"
error: static assertion failed: "PJSUA_MAX_CALLS must be seven"
error: static assertion failed: "PJSUA_MAX_CONF_PORTS must be twelve"
```

These correspond to the old upstream values 8, 4, and 254. The SRTP and TLS
assertions did not fail, confirming that the accepted profile already had
`PJMEDIA_HAS_SRTP=0` and `PJSIP_HAS_TLS_TRANSPORT=0`.

## GREEN implementation and verification

`pjproject/Kconfig` now declares fixed ranges/defaults:

```text
PJSUA_MAX_ACCOUNTS   range/default 5
PJSUA_MAX_CALLS      range/default 7
PJSUA_MAX_CONF_PORTS range/default 12
PJSUA_ARENA_BYTES    range 65536..4194304, default 2097152
```

`config_site.h` maps the three PJSUA limits to Kconfig and sets
`PJSUA_DEFAULT_USE_SRTP` to `PJSUA_SRTP_DISABLED` (the profile alias expands
to the upstream `PJMEDIA_SRTP_DISABLED` value). The explicit link profile sets
all four Kconfig values, including `CONFIG_PJSUA_ARENA_BYTES=2097152`.

The clean sequential build was:

```text
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-task2-ccache2 \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task2-ccache2-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-task2-pjsua2 -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
```

Result: exit 0. Generated configuration confirmed:

```text
CONFIG_PJSUA_MAX_ACCOUNTS=5
CONFIG_PJSUA_MAX_CALLS=7
CONFIG_PJSUA_MAX_CONF_PORTS=12
CONFIG_PJSUA_ARENA_BYTES=2097152
```

The runtime probe command was:

```text
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-task2-ccache2 \
  CCACHE_TEMPDIR=/tmp/voip-plan1-task2-ccache2-tmp \
  timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build \
  -d /tmp/voip-plan1-task2-pjsua2 -t run
```

Result: exit 124 after the required markers:

```text
No SIP worker threads created
PJSUA LINK RESULT: PASSED
```

The initial attempt against an interrupted build directory produced a
zero-length `sip_xfer.c.obj` archive member. That was isolated as a stale
partial build artifact; a clean sequential rebuild in the command above
completed successfully without source changes.

## Runtime mismatch design

`pjsua_config_default()` and `pjsua_media_config_default()` are followed by
assignments from `PJSUA_MAX_CALLS` and `PJSUA_MAX_CONF_PORTS`, respectively.
Before `pjsua_init()`, the probe compares those runtime fields with the same
compile-time constants. A mismatch destroys the created PJSUA instance and
returns failure, so initialization cannot proceed with inconsistent sizing.
The fixed Kconfig values are capacity controls; they do not implement product
account admission, active-call promotion, or FIFO behavior.

## Security-policy evidence

- `pjsua_link.c` asserts `PJMEDIA_HAS_SRTP == 0` and
  `PJSIP_HAS_TLS_TRANSPORT == 0`.
- `config_site.h` retains the existing SRTP/TLS zero profile and maps the
  PJSUA default SRTP mode to disabled.
- `pjsua_link.conf` explicitly keeps `CONFIG_VOIP_SIP_TLS=n`,
  `CONFIG_VOIP_SRTP=n`, `CONFIG_PJMEDIA_SRTP=n`,
  `CONFIG_PJMEDIA_SRTP_TRANSPORT=n`, and `CONFIG_PJMEDIA_SRTP_SDES=n`.
- The profile enables SIP TCP and PJMEDIA UDP/RTP-RTCP, while the probe keeps
  STUN, UPnP, ICE, and TURN disabled at runtime.

## Regression builds and runs

All three builds were run sequentially from the parent west workspace with
absolute worktree application/config paths and writable task-specific cache
paths:

| Profile | Build result | Runtime result |
|---|---:|---|
| `phase3_account.conf` | exit 0 | exit 124; lifecycle 1, 2, 3 and `VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account lifecycles)` |
| `phase5_call.conf` | exit 0 | not run; build regression required by Task 2 |
| `sdk_contract.conf` | exit 0 | exit 124; `VOIP SDK CONTRACT RESULT: PASSED` |

The bounded runtime timeout is expected after each observed marker. No final
phase5 marker is claimed.

## Capacity versus product policy

The `5/7/12` values size native PJSUA account, call-record, and conference-port
arrays. They are not the product topology. The architecture separately limits
the service to five configured agents, two promoted calls, one promoted call
per agent, and a five-entry mixed FIFO. Task 2 implements none of that
scheduling or queue policy; it only freezes the PJSUA capacity consumed later
by those managers.

## Self-review and concerns

`git diff --check` passed. The complete diff from accepted head `650b81d75`
was reviewed for scope: only the four Task 2 implementation files, the
controller ledger, and this report changed. There are no PJSUA2, TLS, SRTP,
arena, worker-thread, manifest, or source-closure changes.

The 2 MiB arena value is declared capacity only; Task 3 still owns allocator
implementation and no heap-elimination claim is made here. QEMU reports high
static RAM for the full PJSUA image (about 3,734,712 bytes, 89.04% of the
4 MiB region), which should be measured again when the arena is implemented.
Existing compiler/Kconfig warnings are unchanged and non-fatal.
