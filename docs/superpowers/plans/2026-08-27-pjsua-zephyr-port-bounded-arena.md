# PJSUA-LIB Zephyr Port and Bounded Arena Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the PJSUA-LIB C API build and run on the existing PJPROJECT Zephyr port with fixed account/call limits, one application-driven event loop, and a dedicated bounded allocation arena.

**Architecture:** Keep upstream PJPROJECT source families explicit in the Zephyr manifests. PJSUA owns standard SIP/account/call/media machinery, but `thread_cnt=0` and the future VoIP actor calls `pjsua_handle_events()`. A Zephyr-specific PJ pool policy serves every PJSUA pool block from one statically reserved, coalescing arena and reports exhaustion as `PJ_ENOMEM`.

**Tech Stack:** C11, PJPROJECT PJSUA-LIB C API, PJLIB pools, PJSIP-UA/SIMPLE, PJNATH, PJMEDIA, Zephyr Kconfig/CMake, ztest-style validation application, QEMU `mps2/an385`.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- Do not add PJSUA2.
- Compile SIP TCP and plain RTP/RTCP UDP; keep SIP TLS and SRTP disabled. PJSUA's link closure still includes PJNATH/ICE/UPnP symbols, but runtime configuration disables STUN, TURN, ICE, and UPnP.
- Set `PJSUA_MAX_ACC=5`, `PJSUA_MAX_CALLS=7`, and `PJSUA_MAX_CONF_PORTS=12`.
- Do not create PJSUA or PJMEDIA worker threads.
- Do not use a general heap after successful service initialization.
- Keep every PJPROJECT source list explicit; do not introduce globs.

---

## Task 1: Add the complete PJSUA link closure

**Files:**

- Modify: `pjproject/Kconfig`
- Modify: `pjproject/zephyr/sources.cmake`
- Modify: `pjproject/zephyr/CMakeLists.txt`
- Modify: `pjproject/pjlib/include/pj/config_site.h`
- Create: `applications/voip_integration/pjsua_link.conf`
- Create: `applications/voip_integration/src/pjsua_link.c`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: existing `pj_zephyr_sources()` manifest helper and PJPROJECT public headers.
- Produces: `CONFIG_PJNATH`, `CONFIG_PJSIP_SIMPLE`, `CONFIG_PJSIP_UA`, and `CONFIG_PJSUA`; linkable `<pjsua-lib/pjsua.h>`.

- [ ] Add a compile/link probe in `pjsua_link.c` that calls `pjsua_create()`, initializes default configs, forces `ua.thread_cnt = 0`, forces `media.thread_cnt = 0`, sets `ua.max_calls = 7`, sets `media.max_media_ports = 12`, calls `pjsua_init()`, `pjsua_set_no_snd_dev()`, `pjsua_start()`, `pjsua_handle_events(0)`, and `pjsua_destroy()`.

- [ ] Add `CONFIG_VOIP_PJSUA_LINK_TEST` and select it in `pjsua_link.conf` with TCP, UDP media, G.711, PJSUA, and audio-device core enabled; explicitly set TLS/SRTP/ICE-related product gates to `n`.

- [ ] Run the link probe before adding manifests and confirm the build fails with unresolved PJSUA symbols:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_pjsua_link -- \
    -DEXTRA_CONF_FILE=pjsua_link.conf
  ```

  Expected: non-zero build result naming `pjsua_create` or its missing dependency closure.

- [ ] Add these exact source families to `pjproject/zephyr/sources.cmake`:

  ```cmake
  pj_zephyr_sources(PJNATH_SOURCES pjnath/src/pjnath
    errno.c ice_session.c ice_strans.c nat_detect.c stun_auth.c
    stun_msg.c stun_msg_dump.c stun_session.c stun_sock.c
    stun_transaction.c turn_session.c turn_sock.c upnp.c)

  pj_zephyr_sources(PJSIP_UA_SOURCES pjsip/src/pjsip-ua
    sip_inv.c sip_reg.c sip_replaces.c sip_xfer.c
    sip_100rel.c sip_timer.c sip_siprec.c)

  pj_zephyr_sources(PJSIP_SIMPLE_SOURCES pjsip/src/pjsip-simple
    errno.c evsub.c evsub_msg.c iscomposing.c mwi.c pidf.c
    dialog_info.c presence.c dlg_event.c presence_body.c
    publishc.c rpid.c xpidf.c)

  pj_zephyr_sources(PJSUA_SOURCES pjsip/src/pjsua-lib
    pjsua_acc.c pjsua_aud.c pjsua_call.c pjsua_core.c pjsua_dump.c
    pjsua_im.c pjsua_media.c pjsua_pres.c pjsua_txt.c pjsua_vid.c)
  ```

- [ ] Extend `PJLIB_UTIL_SOURCES` with the link dependencies used by PJNATH, SIMPLE presence bodies, and UPnP: `crc32.c`, `hmac_sha1.c`, `sha1.c`, `http_client.c`, and `xml.c`; enable the existing `dns.c`, `resolver.c`, and `srv_resolver.c` family whenever PJSUA is selected.

- [ ] Add the PJMEDIA closure required by PJSUA audio/conference and always-compiled media APIs to the same manifest: `audiodev.c`, `bidirectional.c`, `clock_thread.c`, `conference.c`, `converter.c`, `delaybuf.c`, `echo_common.c`, `echo_port.c`, `echo_suppress.c`, `master_port.c`, `mem_capture.c`, `mem_player.c`, `null_port.c`, `resample_port.c`, `session.c`, `sound_port.c`, `splitcomb.c`, `stereo_port.c`, `tonegen.c`, `transport_ice.c`, `txt_stream.c`, `wav_player.c`, `wav_playlist.c`, `wav_writer.c`, and `wave.c`; add `pjmedia-audiodev/audiodev.c`, `errno.c`, and `null_dev.c` as `PJMEDIA_AUDIODEV_SOURCES`.

- [ ] Wire four distinct Zephyr libraries in dependency order: `pjnath -> pjlib-util,pjlib`, `pjsip-simple -> pjsip,pjlib-util,pjlib`, `pjsip-ua -> pjsip-simple,pjsip,pjmedia`, and `pjsua -> pjsip-ua,pjsip-simple,pjsip,pjmedia,pjnath`. Do not merge PJSUA files into the current `pjsip` target.

- [ ] In `config_site.h`, set the initial embedded feature profile: `PJMEDIA_HAS_VIDEO=0`, `PJMEDIA_HAS_SRTP=0`, `PJSIP_HAS_TLS_TRANSPORT=0`, and all host audio backends to zero. Preserve PJNATH/ICE types and link symbols required by unmodified PJSUA, while setting runtime account/global configuration to disable STUN, TURN, ICE, and UPnP.

- [ ] Rebuild until the explicit source closure links without adding a glob or PJSUA2:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_pjsua_link -- \
    -DEXTRA_CONF_FILE=pjsua_link.conf
  ```

  Expected: successful compile and link.

- [ ] Run the image:

  ```sh
  west build -d build_voip_pjsua_link -t run
  ```

  Expected console terminator: `PJSUA LINK RESULT: PASSED`; QEMU may then idle and be stopped.

- [ ] Commit:

  ```sh
  git add pjproject/Kconfig pjproject/zephyr pjproject/pjlib/include/pj/config_site.h applications/voip_integration
  git commit -m "feat(pjproject): port pjsua-lib to Zephyr"
  ```

## Task 2: Freeze embedded PJSUA limits and unsupported security policies

**Files:**

- Modify: `pjproject/Kconfig`
- Modify: `pjproject/pjlib/include/pj/config_site.h`
- Modify: `applications/voip_integration/src/pjsua_link.c`
- Modify: `applications/voip_integration/pjsua_link.conf`

**Interfaces:**

- Consumes: PJSUA compile-time limits and Kconfig values.
- Produces: a reproducible five-account/seven-call/twelve-port build profile.

- [ ] Add failing `_Static_assert` checks to `pjsua_link.c` for `PJSUA_MAX_ACC == 5`, `PJSUA_MAX_CALLS == 7`, `PJSUA_MAX_CONF_PORTS == 12`, `PJMEDIA_HAS_SRTP == 0`, and `PJSIP_HAS_TLS_TRANSPORT == 0`.

- [ ] Build and confirm the assertions fail against current defaults (`PJSUA_MAX_ACC=8`, `PJSUA_MAX_CALLS=4`, `PJSUA_MAX_CONF_PORTS=254`).

- [ ] Add fixed Kconfig integers `PJSUA_MAX_ACCOUNTS` (default/range 5), `PJSUA_MAX_CALLS` (default/range 7), `PJSUA_MAX_CONF_PORTS` (default/range 12), and `PJSUA_ARENA_BYTES` (range `65536 4194304`, initial QEMU default `2097152`). They are build-time sizing controls, not runtime topology controls; Plan 6 selects the measured target value.

- [ ] Map them in `config_site.h`:

  ```c
  #define PJSUA_MAX_ACC        CONFIG_PJSUA_MAX_ACCOUNTS
  #define PJSUA_MAX_CALLS      CONFIG_PJSUA_MAX_CALLS
  #define PJSUA_MAX_CONF_PORTS CONFIG_PJSUA_MAX_CONF_PORTS
  #define PJSUA_DEFAULT_USE_SRTP PJSUA_SRTP_DISABLED
  ```

- [ ] In the runtime probe, set `pjsua_config.max_calls` and `pjsua_media_config.max_media_ports` to the same constants and reject any mismatch before calling `pjsua_init()`.

- [ ] Rebuild and run; confirm all compile-time and runtime checks pass.

- [ ] Commit:

  ```sh
  git add pjproject/Kconfig pjproject/pjlib/include/pj/config_site.h applications/voip_integration
  git commit -m "feat(pjsua): freeze embedded resource limits"
  ```

## Task 3: Replace PJ pool block allocation with a static arena

**Files:**

- Create: `pjproject/zephyr/include/pj_zephyr_pool_arena.h`
- Create: `pjproject/zephyr/pj_zephyr_pool_arena.c`
- Modify: `pjproject/zephyr/CMakeLists.txt`
- Create: `applications/voip_integration/src/pjsua_arena_test.c`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Create: `applications/voip_integration/pjsua_arena.conf`

**Interfaces:**

- Consumes: `pj_pool_factory_default_policy`, `CONFIG_PJSUA_ARENA_BYTES`, Zephyr spinlock API.
- Produces: `pj_zephyr_pool_arena_install()`, `pj_zephyr_pool_arena_reset()`, and `pj_zephyr_pool_arena_get_stats()`.

- [ ] Define the public C contract:

  ```c
  struct pj_zephyr_pool_arena_stats {
      size_t capacity_bytes;
      size_t used_bytes;
      size_t peak_bytes;
      size_t largest_free_block;
      uint32_t live_blocks;
      uint32_t failed_allocations;
  };

  pj_status_t pj_zephyr_pool_arena_install(void);
  pj_status_t pj_zephyr_pool_arena_reset(void);
  void pj_zephyr_pool_arena_get_stats(
      struct pj_zephyr_pool_arena_stats *out);
  ```

- [ ] Write a failing arena test that installs the policy, allocates different sized PJ pools until exhaustion, expects the next allocation to return `NULL`/`PJ_ENOMEM`, frees alternating pools, verifies coalescing, frees all pools, and verifies `used_bytes == 0` and `live_blocks == 0`.

- [ ] Build and confirm failure due to missing arena symbols.

- [ ] Implement one `alignas(max_align_t)` byte array of `CONFIG_PJSUA_ARENA_BYTES`, boundary-tag blocks, aligned first-fit allocation, block splitting, adjacent free-block coalescing, overflow checks, and a `k_spinlock`. Block allocation must never fall back to `malloc()`.

- [ ] Install the arena by replacing the three callbacks in `pj_pool_factory_default_policy` before `pjsua_create()`. The out-of-memory callback must record the failure and return control through PJPROJECT's normal `PJ_ENOMEM` path; it must not abort the device.

- [ ] Make `reset()` return `PJ_EBUSY` while `live_blocks != 0`; scrub allocator metadata on successful reset.

- [ ] Run the standalone arena test and confirm deterministic exhaustion and full recovery for 100 cycles.

- [ ] Commit:

  ```sh
  git add pjproject/zephyr applications/voip_integration
  git commit -m "feat(pjlib): add bounded Zephyr pool arena"
  ```

## Task 4: Prove PJSUA uses only the arena and no worker thread

**Files:**

- Modify: `applications/voip_integration/src/pjsua_link.c`
- Modify: `applications/voip_integration/pjsua_link.conf`

**Interfaces:**

- Consumes: arena statistics and PJSUA lifecycle API.
- Produces: repeated PJSUA lifecycle and event-loop proof.

- [ ] Extend the probe to install/reset the arena, capture statistics before `pjsua_create()`, after `pjsua_start()`, and after `pjsua_destroy()`, and execute five complete lifecycles.

- [ ] Add a failing assertion that the post-destroy state has zero live arena blocks and that peak usage is non-zero but no larger than capacity.

- [ ] Configure `ua.thread_cnt = 0`, `media.thread_cnt = 0`, `media.has_ioqueue = PJ_FALSE`, and `media.conf_threads = 1`; use the caller loop to invoke `pjsua_handle_events(10)`.

- [ ] Record the actor thread ID before initialization and assert every registered PJSUA callback executes on that same thread during this probe.

- [ ] Run five cycles and require `PJSUA LINK RESULT: PASSED (5 lifecycles, arena clean)`.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(pjsua): prove actor-driven bounded lifecycle"
  ```

## Task 5: Validate five account records and seven call-record exhaustion behavior

**Files:**

- Create: `applications/voip_integration/src/pjsua_capacity.c`
- Create: `applications/voip_integration/pjsua_capacity.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Interfaces:**

- Consumes: PJSUA account/call APIs and loop/TCP test transport.
- Produces: capacity proof matching the product scheduler's worst case.

- [ ] Add a test that creates five local accounts with `pjsua_acc_add(..., PJ_FALSE, ...)`, assigns distinct user-data sentinels, enumerates them, and verifies the sentinels through `pjsua_acc_get_user_data()`.

- [ ] Add a controlled incoming-call harness that holds seven lightweight PJSUA call records without media, then injects an eighth INVITE.

- [ ] Run before setting limits and confirm the test fails at the fourth/fifth call under the old `PJSUA_MAX_CALLS=4` default.

- [ ] With Task 2 limits active, verify seven records remain independently addressable and the eighth receives SIP `486 Busy Here`, matching PJSUA's `alloc_call_id()` failure behavior.

- [ ] Destroy all seven calls and five accounts and verify the arena returns to its post-start baseline before final PJSUA destruction.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(pjsua): validate embedded account and call capacities"
  ```

## Task 6: Record port footprint and acceptance commands

**Files:**

- Create: `docs/voip/pjsua-zephyr-port.md`
- Modify: `docs/voip/pjsua-zephyr-port.md` after measurement only

**Interfaces:**

- Consumes: build output, map file, arena stats.
- Produces: reproducible port acceptance record.

- [ ] Document the exact selected source families, disabled features, Kconfig limits, arena ownership, and why seven PJSUA call IDs are required for two promoted plus five queued incoming calls.

- [ ] Perform pristine builds for `pjsua_link.conf`, `pjsua_arena.conf`, and `pjsua_capacity.conf`; paste commands and final pass markers into the document.

- [ ] Record ROM, static RAM, `CONFIG_PJSUA_ARENA_BYTES`, observed arena peak, thread count, and largest actor stack watermark from build/runtime output. Do not invent threshold values; record measured values and link their acceptance to Plan 6 qualification.

- [ ] Run:

  ```sh
  git diff --check
  ```

  Expected: no whitespace errors.

- [ ] Commit:

  ```sh
  git add docs/voip/pjsua-zephyr-port.md
  git commit -m "docs(pjsua): record Zephyr port acceptance"
  ```

## Plan 1 Exit Criteria

- PJSUA-LIB C API links without PJSUA2.
- Five accounts, seven PJSUA call records, and twelve conference ports are compile-time bounded.
- `pjsua_handle_events()` is driven by the caller; PJSUA creates no worker thread.
- All PJ pool blocks used by PJSUA come from the fixed arena.
- Arena exhaustion is reported and teardown restores a clean arena.
- TLS, SRTP, and video remain compile-time disabled; STUN, TURN, ICE, and UPnP remain runtime disabled even though PJSUA's required PJNATH/ICE link closure is present.
