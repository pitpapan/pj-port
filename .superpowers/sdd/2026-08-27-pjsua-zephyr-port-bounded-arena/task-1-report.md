# Task 1 report — complete PJSUA link closure

## Summary

Task 1 is complete on `codex/pjsua-port-plan1`. The Zephyr PJPROJECT port now
builds four distinct libraries (`pjnath`, `pjsip-simple`, `pjsip-ua`, and
`pjsua`) with explicit source lists and links the C `<pjsua-lib/pjsua.h>`
probe. The probe uses no SIP worker threads, selects the Zephyr no-thread
PJMEDIA event-manager path, disables STUN/TURN/ICE/UPnP at runtime, uses the
null sound device, and prints the required pass marker. TLS, SRTP, video, and
host audio backends remain disabled. PJSUA now declares its mandatory PJMEDIA
closure in Kconfig, while the two PJSUA-only STUN helper sources remain out of
minimal PJLIB-UTIL profiles.

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
was added. The STUN helpers now live in the PJSUA-gated
`PJLIB_UTIL_PJSUA_SOURCES` family rather than the unconditional PJLIB-UTIL
list.

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

## Review-fix GREEN and regressions

The valid `pjsua_link.conf` was rebuilt after the review fixes with the same
absolute application/config pattern above (build directory
`/tmp/voip-plan1-pjsua-review`): it exited 0, generated the four PJSUA-related
libraries, and reported FLASH 525280 B / RAM 3746104 B. Its bounded QEMU run
exited 124 after idle QEMU, after printing both `No SIP worker threads
created` and `PJSUA LINK RESULT: PASSED`. This is a Zephyr compile/runtime
check of the gated `pjsua_media.c` path. The code-path check is intentionally
limited: it does not enumerate PJMEDIA threads; Task 4 owns explicit thread
enumeration/qualification.

The non-Zephyr behavior is preserved by the preprocessor guard
`defined(PJ_ZEPHYR) && PJ_ZEPHYR!=0`; non-Zephyr builds retain the upstream
zero event-manager options. No non-Zephyr build was required for this Zephyr
port review.

The PJSUA helper relocation was checked against the minimal profiles below:
their PJLIB-UTIL build output contains no `stun_simple_client.c` or
`stun_simple.c`, while the full PJSUA build compiles both from the gated
family. The full PJSUA build also compiled the helper and linked successfully.

Minimal-profile commands and results:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-phase3-clean -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/phase3_account.conf

env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-phase5-call -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/phase5_call.conf
```

Both builds exited 0. Phase 3 generated FLASH 170196 B / RAM 560240 B and
its bounded run emitted `VOIP INTEGRATION PHASE 3 RESULT: PASSED (3 account
lifecycles)`, then exited 124 after idle QEMU. Phase 5 generated FLASH
257948 B / RAM 642088 B. Its bounded 30-second run emitted
`[Phase 5] lifecycle 1 TCP call-control matrix: PASSED`, showed SIP 486
responses for the busy-call path, and exited 124 while the harness continued
its later lifecycle; this existing harness does not reach its final marker
within the 30-second bound.

The generated minimal-profile ownership evidence has no duplicate translation
units: phase 3's `pjsip` archive contains `sip_reg.c`; phase 5's `pjsip`
archive contains `sip_inv.c` and `sip_reg.c`. With `CONFIG_PJSIP_UA=y`, the
PJSIP CMake branch omits both feature families from `pjsip` and the full build
places them only in `pjsip-ua` (the review build output showed
`CMakeFiles/pjsip-ua.dir/.../sip_inv.c.obj` and `sip_reg.c.obj`).

No `PJSUA2`, TLS, SRTP, or source glob was introduced.

The Kconfig contract RED used `/tmp/voip-plan1-incomplete.conf`, setting
`CONFIG_PJSUA=y` while disabling SDP negotiation, endpoint, G.711, RTP/RTCP,
UDP transport, stream, and audio-device gates. Before the dependency fix, the
same profile reached final link and exited 1 on missing PJMEDIA symbols. With
the fix, it exited 1 during Kconfig/CMake generation: Kconfig reported
`PJSUA` assigned `y` but resolved `n`, naming each missing mandatory gate, and
the application target had no sources. This is the expected configuration
failure rather than an unsafe link attempt.

The exact post-fix guard command was:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-incomplete-green -- \
  -DEXTRA_CONF_FILE=/tmp/voip-plan1-incomplete.conf
```

## Regression evidence

Pristine SDK contract build:

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  /home/pitpapan/zephyrproject/.venv/bin/west build -p always \
  -b mps2/an385 \
  /home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration \
  -d /tmp/voip-plan1-sdk-clean -- \
  -DEXTRA_CONF_FILE=/home/pitpapan/zephyrproject/.worktrees/voip-pjsua-plan1/applications/voip_integration/sdk_contract.conf
```

Result: exit 0 (FLASH 21160 B, RAM 24912 B).

```sh
env PATH=/home/pitpapan/zephyrproject/.venv/bin:/usr/bin:/bin \
  CCACHE_DIR=/tmp/voip-plan1-ccache \
  CCACHE_TEMPDIR=/tmp/voip-plan1-ccache-tmp \
  timeout 30s /home/pitpapan/zephyrproject/.venv/bin/west build \
  -d /tmp/voip-plan1-sdk-clean -t run
```

Result: `VOIP SDK CONTRACT RESULT: PASSED`; QEMU idled afterward and the
bounded command returned 124.

## Decisions and self-review

- INVITE/REGC sources remain available to minimal profiles, but ownership
  moves to `pjsip-ua` whenever that full boundary is selected, preventing
  duplicate translation units.
- The `pjsua_media.c` change is minimal and Zephyr-only: with
  `thread_cnt == 0` and no media ioqueue it passes
  `PJMEDIA_EVENT_MGR_NO_THREAD`, avoiding an otherwise-created PJMEDIA event
  worker. The successful Zephyr lifecycle and `No SIP worker threads created`
  log prove the selected code path and lifecycle; they do not enumerate
  PJMEDIA workers. Task 4 owns that explicit thread proof.
- `CONFIG_PJSUA` now depends on every PJMEDIA gate required by the linked
  PJSUA closure, instead of permitting an incomplete configuration to reach
  link. The required closure is expressed with `depends on`, not `select`.
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
