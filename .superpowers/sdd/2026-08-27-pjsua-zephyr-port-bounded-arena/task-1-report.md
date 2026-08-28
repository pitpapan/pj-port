# Task 1 report — complete PJSUA link closure

## Summary

Task 1 is complete on `codex/pjsua-port-plan1`. The Zephyr PJPROJECT port now
builds four distinct libraries (`pjnath`, `pjsip-simple`, `pjsip-ua`, and
`pjsua`) with explicit source lists and links the C `<pjsua-lib/pjsua.h>`
probe. The probe uses no SIP or media worker threads, disables STUN/TURN/ICE/
UPnP at runtime, uses the null sound device, and prints the required pass
marker. TLS, SRTP, video, and host audio backends remain disabled.

## Exact files

Changed or added for this task:

- `pjproject/Kconfig`
- `pjproject/zephyr/sources.cmake`
- `pjproject/zephyr/CMakeLists.txt`
- `pjproject/pjlib/include/pj/config_site.h`
- `pjproject/pjsip/src/pjsua-lib/pjsua_media.c`
- `applications/voip_integration/Kconfig`
- `applications/voip_integration/CMakeLists.txt`
- `applications/voip_integration/pjsua_link.conf`
- `applications/voip_integration/src/pjsua_link.c`
- `.superpowers/sdd/2026-08-27-pjsua-zephyr-port-bounded-arena/progress.md`
- this report

No files under `zephyr/` were inspected or changed.

## RED evidence

To safely reconstruct the missing pre-manifest evidence, an isolated copy was
made under `/tmp/voip-plan1-red` and configured with the probe and feature
symbols but without the PJSUA source manifests. The parent workspace was used
for west, as required:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-red-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-red-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 /tmp/voip-plan1-red/applications/voip_integration \
  -d /tmp/voip-plan1-red-build -- \
  -DEXTRA_CONF_FILE=pjsua_link.conf
```

The build exited 1 at final link. The diagnostic named undefined
`pjsua_create`, `pjsua_config_default`, `pjsua_logging_config_default`,
`pjsua_media_config_default`, `pjsua_init`, `pjsua_set_no_snd_dev`,
`pjsua_start`, `pjsua_handle_events`, and `pjsua_destroy`. This is the
expected missing PJSUA closure failure.

During closure audit, omitting `stun_simple_client.c` and `stun_simple.c`
from an otherwise complete build produced the concrete linker error
`undefined reference to pjstun_get_mapped_addr2` at `pjsua_media.c:596` and
`:662`; both files are therefore retained explicitly. A second isolated
omission of `resample_resample.c` and `audio_codecs.c` produced undefined
references to `pjmedia_resample_create`, `pjmedia_resample_destroy`,
`pjmedia_resample_run`, `pjmedia_audio_codec_config_default`, and
`pjmedia_codec_register_audio_codecs`; those two files are retained as
explicit PJSUA media closure dependencies. No glob or broad source expansion
was added.

## GREEN evidence

Final pristine build (absolute config path is needed when the build directory
is newly created and the application source is absolute):

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-pjsua-link -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/pjsua_link.conf
```

Result: exit 0. The link command included `libpjnath.a`,
`libpjsip-simple.a`, `libpjsip-ua.a`, and `libpjsua.a`; the final image was
generated successfully (FLASH 525280 B, RAM 3746104 B on `mps2/an385`).

Runtime command:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build \
  -d /tmp/voip-plan1-pjsua-link -t run
```

Result: `PJSUA LINK RESULT: PASSED` and `No SIP worker threads created` were
printed. QEMU then idled; `timeout` returned 124 after the marker. There was
no abort or fault in the final run.

The probe explicitly disables unsolicited MWI. Debug instrumentation showed
the default MWI module path aborted an empty-account lifecycle during
`pjsua_start()`; disabling it is appropriate for this account-free link
probe and leaves presence APIs in the linked closure.

## Regression evidence

Pristine SDK contract build:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-sdk-contract -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/sdk_contract.conf
```

Result: exit 0 (FLASH 21160 B, RAM 24912 B).

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build \
  -d /tmp/voip-plan1-sdk-contract -t run
```

Result: `VOIP SDK CONTRACT RESULT: PASSED`; QEMU idled afterward and the
bounded command returned 124.

## Decisions and self-review

- INVITE/REGC sources remain available to minimal profiles, but ownership
  moves to `pjsip-ua` whenever that full boundary is selected, preventing
  duplicate translation units.
- The `pjsua_media.c` change is minimal and necessary: with
  `thread_cnt == 0` and no media ioqueue it passes
  `PJMEDIA_EVENT_MGR_NO_THREAD`, avoiding an otherwise-created PJMEDIA event
  worker. The final runtime confirms the lifecycle reaches RUNNING without
  worker-thread creation.
- `config_site.h` keeps `PJMEDIA_HAS_VIDEO=0`, `PJMEDIA_HAS_SRTP=0`,
  `PJSIP_HAS_TLS_TRANSPORT=0`, and all host audio backends at zero. Runtime
  STUN/TURN/ICE/UPnP settings are disabled in `pjsua_link.c`.
- All PJPROJECT source manifests remain explicit. PJSUA2, TLS, SRTP, and
  source globs were not added.
- `git diff --check` is clean.

## Concerns for later tasks

The compiler emits existing upstream warnings in PJSIP-UA TODO labels and the
PJSUA thread-name `snprintf` bound; they do not fail the build. Task 2 still
owns compile-time `5/7/12` assertions and sizing, Task 3 owns bounded arena
allocation, and later tasks must add meaningful callback and capacity tests.
