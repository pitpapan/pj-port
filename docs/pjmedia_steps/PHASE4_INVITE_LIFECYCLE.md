# Phase 4 — PJSIP INVITE Lifecycle

## Goal in plain language

Make the PJSIP INVITE, 100rel, and session-timer modules compile, initialize,
and shut down repeatedly with the Phase 3 SDP negotiator.

Do not send an INVITE in this phase. Do not open a network socket. Do not add
RTP, a PJMEDIA endpoint, codecs, streams, or audio devices.

This is PJMEDIA port Phase 4. It is unrelated to the older validation file
named `applications/pjsip_minimal/phase4_core.conf`.

## Prerequisite

Phase 3 must still print:

```text
PHASE 3 RESULT: PASSED (3 complete negotiation lifecycles)
```

Its archive must contain the seven validated SDP/negotiation objects recorded
in `docs/PJMEDIA_PHASE3_VALIDATION.md`.

## Files to change

Production integration:

```text
pjproject/zephyr/CMakeLists.txt
```

Validation application:

```text
applications/pjmedia_minimal/Kconfig
applications/pjmedia_minimal/CMakeLists.txt
applications/pjmedia_minimal/src/main.c
applications/pjmedia_minimal/phase4_invite.conf              new
applications/pjmedia_minimal/src/phase4_invite.c              new
```

`CONFIG_PJSIP_INVITE` already exists in `pjproject/Kconfig`; do not create a
second production symbol.

## Step 1 — Add the three-source production group

Open `pjproject/zephyr/CMakeLists.txt`. In the `CONFIG_PJSIP` section, define:

```cmake
set(PJSIP_INVITE_SOURCES
  ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip-ua/sip_inv.c
  ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip-ua/sip_100rel.c
  ${PJPROJECT_ROOT_DIR}/pjsip/src/pjsip-ua/sip_timer.c
)
```

After `zephyr_library_named(pjsip)`, activate it conditionally:

```cmake
zephyr_library_sources_ifdef(CONFIG_PJSIP_INVITE
  ${PJSIP_INVITE_SOURCES}
)
```

When `CONFIG_PJSIP_INVITE` is enabled, the PJSIP target also needs the
PJMEDIA public include directory and the validated `pjmedia` library:

```cmake
if(CONFIG_PJSIP_INVITE)
  zephyr_library_include_directories(
    ${PJPROJECT_ROOT_DIR}/pjmedia/include
  )
  target_link_libraries(pjsip PUBLIC pjmedia)
endif()
```

Do not add these optional sources:

```text
sip_replaces.c
sip_siprec.c
sip_xfer.c
```

Do not add PJSUA-LIB, PJSIP-SIMPLE, PJNATH, RTP, or audio sources to fix a
link failure.

## Step 2 — Add the Phase 4 test selector

Add `PJMEDIA_PHASE4_INVITE_TEST` to
`applications/pjmedia_minimal/Kconfig`. It should depend on:

```text
PJPROJECT
PJSIP
PJSIP_INVITE
PJMEDIA_SDP_NEG
```

It should also require later media families to remain disabled:

```text
!PJMEDIA_ENDPOINT
!PJMEDIA_G711
!PJMEDIA_RTP_RTCP
!PJMEDIA_UDP_TRANSPORT
!PJMEDIA_STREAM
!PJMEDIA_AUDIODEV
```

Add `src/phase4_invite.c` conditionally in the application CMake file and add
`phase4_invite_run()` dispatch before the Phase 3 branches in `main.c`.

## Step 3 — Create the configuration overlay

Create `applications/pjmedia_minimal/phase4_invite.conf` with this boundary:

```ini
CONFIG_PJLIB_UTIL=y
CONFIG_PJMEDIA=y
CONFIG_PJMEDIA_SDP=y
CONFIG_PJMEDIA_SDP_NEG=y
CONFIG_PJSIP=y
CONFIG_PJSIP_INVITE=y

CONFIG_PJMEDIA_ENDPOINT=n
CONFIG_PJMEDIA_G711=n
CONFIG_PJMEDIA_RTP_RTCP=n
CONFIG_PJMEDIA_UDP_TRANSPORT=n
CONFIG_PJMEDIA_STREAM=n
CONFIG_PJMEDIA_AUDIODEV=n
CONFIG_PJSIP_UDP_TRANSPORT=n
CONFIG_PJSIP_TCP_TRANSPORT=n

CONFIG_PJMEDIA_PHASE1_BOUNDARY_TEST=n
CONFIG_PJMEDIA_PHASE2_SDP_TEST=n
CONFIG_PJMEDIA_PHASE3_SDP_NEG_TEST=n
CONFIG_PJMEDIA_PHASE3_LINK_PROBE=n
CONFIG_PJMEDIA_PHASE4_INVITE_TEST=y

CONFIG_HEAP_MEM_POOL_SIZE=262144
CONFIG_MAIN_STACK_SIZE=32768
CONFIG_INIT_STACKS=y
CONFIG_THREAD_STACK_INFO=y
```

The heap and stack are validation starting values. Measure actual peaks before
making a smaller production recommendation.

## Step 4 — First checkpoint: compile only

Create a minimal `phase4_invite.c` that includes the public PJLIB, PJSIP-UA,
PJMEDIA SDP, and SDP-negotiation headers and prints a temporary compile-boundary
marker. Then run:

```sh
source .venv/bin/activate
source zephyr/zephyr-env.sh
export CCACHE_DISABLE=1
export CMAKE_BUILD_PARALLEL_LEVEL=1

west build -p always -b mps2/an385 applications/pjmedia_minimal \
  -d build-pjmedia-phase4 -- -DEXTRA_CONF_FILE=phase4_invite.conf
```

Stop here if compilation or linking fails. Audit each missing symbol before
adding a source. A missing symbol is not permission to add the full PJSIP-UA,
PJMEDIA, or PJSUA source tree.

Expected new PJSIP archive objects:

```text
sip_inv.c.obj
sip_100rel.c.obj
sip_timer.c.obj
```

## Step 5 — Implement one module lifecycle

Use the endpoint lifecycle in
`applications/pjsip_minimal/src/phase5_core.c` as a reference, but put the new
test in `applications/pjmedia_minimal`.

Initialize in this order:

```text
pj_init()
pjlib_util_init()
pj_caching_pool_init()
pjsip_endpt_create()
pjsip_tsx_layer_init_module()
pjsip_ua_init_module()
pjsip_100rel_init_module()
pjsip_timer_init_module()
pjsip_inv_usage_init()
```

Use a zero-initialized `pjsip_ua_init_param` for the UA layer.

Use a zero-initialized `pjsip_inv_callback` for INVITE usage. At minimum, set
the mandatory callback:

```c
static void on_state_changed(pjsip_inv_session *inv, pjsip_event *event)
{
	PJ_UNUSED_ARG(inv);
	PJ_UNUSED_ARG(event);
}
```

```c
pj_bzero(&inv_cb, sizeof(inv_cb));
inv_cb.on_state_changed = &on_state_changed;
```

Do not call `pjsip_inv_usage_init()` with a missing mandatory callback in a
debug build: PJPROJECT treats that as a precondition violation and asserts.
Validate the callback structure locally before calling the API.

For the first checkpoint, destroy the endpoint after module initialization,
destroy the caching pool, call `pj_shutdown()`, and require zero checked-out
pools.

## Step 6 — Add UAC and UAS create/destroy tests

After one module lifecycle works:

1. Parse a manual PCMU/PCMA/telephone-event SDP using the Phase 3 helper
   pattern.
2. Create a UAC dialog with `pjsip_dlg_create_uac()`.
3. Create a UAC INVITE session with `pjsip_inv_create_uac()`.
4. Release it without sending a call, using the documented reference and
   dialog teardown APIs.
5. Construct a valid in-memory incoming INVITE request using the already
   validated PJSIP parser/message helpers.
6. Create the UAS dialog with `pjsip_dlg_create_uas_and_inc_lock()`; avoid the
   deprecated `pjsip_dlg_create_uas()` API.
7. Create a UAS session with `pjsip_inv_create_uas()`.
8. Release the UAS session, dialog lock, and request storage.

Track every reference and lock explicitly. A successful create followed by a
leak is a failed test.

## Step 7 — Add lifecycle and reset coverage

Repeat the complete sequence at least three times:

```text
PJLIB init
endpoint creation
transaction and UA module initialization
100rel, timer, and INVITE usage initialization
UAC create/destroy
UAS create/destroy
endpoint destruction
pool and PJLIB shutdown
```

Also test cleanup when initialization stops partway through. Use failure
injection in the harness or a deliberately controlled resource failure; do not
violate API preconditions.

After endpoint recreation, verify that INVITE/UA module IDs and session-timer
parser/global state can be initialized again without `PJ_EINVALIDOP`, duplicate
registration, or stale callbacks.

## Step 8 — Run and audit

```sh
timeout --signal=TERM --kill-after=5s 30s \
  west build -d build-pjmedia-phase4 -t run
```

Required final marker:

```text
PHASE 4 RESULT: PASSED (3 complete INVITE module lifecycles)
```

Audit the PJSIP archive, PJMEDIA archive, final ELF, configuration, and QEMU
process. The only new production objects should be the three Phase 4 PJSIP-UA
objects. The PJMEDIA archive should remain the Phase 3 seven-object closure.

## Step 9 — Regressions

Rerun:

1. PJMEDIA Phase 3 negotiation;
2. existing PJSIP registration and OPTIONS validation;
3. native/root CMake diff and whitespace checks.

Phase 4 has passed only when existing signaling remains usable after repeated
INVITE-module initialization and teardown.

## Done checklist

- [ ] Exactly three PJSIP-UA sources were added.
- [ ] No optional PJSIP-UA/PJSUA/PJNATH/media-runtime source was added.
- [ ] Module initialization order is deterministic.
- [ ] Mandatory callbacks are present.
- [ ] UAC and UAS sessions create and destroy without sending a call.
- [ ] Three complete lifecycles leave zero live pools/resources.
- [ ] Module/parser global state resets across endpoint recreation.
- [ ] Phase 3 and PJSIP signaling regressions pass.
- [ ] Phase 5 was not started.
