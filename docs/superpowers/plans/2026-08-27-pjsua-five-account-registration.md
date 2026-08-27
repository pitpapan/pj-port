# PJSUA Five-Account Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect the core service to one actor-owned PJSUA runtime and independently manage registration for one to five initialization-time agent accounts over plain SIP TCP.

**Architecture:** `PjsuaRuntime` owns lifecycle and event polling; `PjsuaTransportManager` owns the one TCP transport; `PjsuaCallbackRouter` forwards static C callbacks to managers on the actor; `PjsuaAccountManager` owns five PJ-specific contexts. Each account context stores only `pjsua_acc_id`, its stable `AgentHandle`, registration state, and retry state. Agent credentials and audio bindings remain owned by `AgentRegistry`.

**Tech Stack:** C++17, PJSUA-LIB C API, SIP TCP, Digest authentication, fixed arrays, Zephyr actor thread, fake PJSUA function table for host tests, in-process scripted SIP registrar for QEMU integration.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

## Global Constraints

- Never inspect, search, index, or modify `zephyr/`.
- Use PJSUA-LIB C API, not PJSUA2 or a new low-level registration client.
- Configure all accounts only during transactional service initialization.
- Use one PJSUA runtime, one SIP TCP transport, and zero PJSUA worker threads.
- Registration failure for one agent must not stop or corrupt another.
- Do not add TLS; reject its policy as unsupported.
- Do not move audio device ownership into `PjsuaAccountContext`.

---

## Task 1: Build the actor-owned PJSUA runtime and transport manager

**Files:**

- Create: `voip/src/pjsua/PjsuaApi.hpp`
- Create: `voip/src/pjsua/PjsuaRuntime.hpp`
- Create: `voip/src/pjsua/PjsuaRuntime.cpp`
- Create: `voip/src/pjsua/PjsuaTransportManager.hpp`
- Create: `voip/src/pjsua/PjsuaTransportManager.cpp`
- Create: `voip/src/pjsua/SignalingTransportPolicy.hpp`
- Create: `voip/tests/pjsua/FakePjsuaApi.hpp`
- Create: `voip/tests/pjsua/PjsuaRuntimeTest.cpp`

**Interfaces:**

- Consumes: Plan 1 arena hook and PJSUA lifecycle/transport APIs.
- Produces: `PjsuaRuntime::Initialize()`, `Poll(timeout_ms)`, `Shutdown()`, and native-endpoint access restricted to the pjsua directory.

- [ ] Write a fake-API test that checks exact lifecycle order: arena install/reset, `pjsua_create`, defaults, `pjsua_init`, TCP transport create, no-sound selection, `pjsua_start`, explicit polls, transport close, `pjsua_destroy`, arena reset.

- [ ] Add failure injection after every initialization call and assert rollback executes only acquired stages in reverse order and is idempotent.

- [ ] Confirm the test fails because `PjsuaRuntime` is absent.

- [ ] Define a narrow `PjsuaApi` function table for used C calls so host tests can inject deterministic status/callbacks without emulating PJPROJECT internals.

- [ ] In `PjsuaRuntime::Initialize()`, set:

  ```cpp
  ua.max_calls = 7;
  ua.thread_cnt = 0;
  media.thread_cnt = 0;
  media.has_ioqueue = PJ_FALSE;
  media.max_media_ports = 12;
  ua.use_srtp = PJSUA_SRTP_DISABLED;
  ```

- [ ] Set `ua.stun_srv_cnt = 0`, disable per-account STUN/ICE/UPnP use, select no sound device, and create only `PJSIP_TRANSPORT_TCP`. Return `unsupported_configuration` before PJSUA initialization for TLS.

- [ ] Record the actor thread identity at initialize. Assert in debug builds that `Poll()`, shutdown, and all mutable manager entry points execute on it.

- [ ] Implement `Poll()` solely as `pjsua_handle_events(timeout_ms)`. A negative PJ status becomes a runtime fault notification; zero is not an error.

- [ ] Run `PjsuaRuntimeTest` and require success for normal lifecycle and every injected failure point.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua
  git commit -m "feat(voip): add actor-owned pjsua runtime"
  ```

## Task 2: Route PJSUA callbacks without singleton domain mutation

**Files:**

- Create: `voip/src/pjsua/PjsuaCallbackRouter.hpp`
- Create: `voip/src/pjsua/PjsuaCallbackRouter.cpp`
- Create: `voip/tests/pjsua/PjsuaCallbackRouterTest.cpp`

**Interfaces:**

- Consumes: `pjsua_callback`, PJSUA account/call user-data APIs.
- Produces: static callback table and instance routing to account/call/media managers.

- [ ] Write tests that install a router, inject registration callbacks for two account IDs, verify manager routing, verify callbacks after quiescence are ignored safely, and verify wrong-thread callbacks trip the debug guard without invoking the application.

- [ ] Confirm the tests fail because the router is absent.

- [ ] Populate only callbacks used by the product: `on_reg_state2`, `on_incoming_call`, `on_call_state`, `on_call_media_state`, `on_call_sdp_created`, `on_stream_created2`, and `on_stream_destroyed`.

- [ ] Use PJSUA account/call user data as the primary context lookup. The process-global static trampoline may point to the sole active `PjsuaCallbackRouter`, but it may only forward; all mutable state remains in the runtime instance. Reject a second active runtime with `invalid_state`.

- [ ] Add `BeginQuiescence()` before account/call teardown and `Detach()` only after PJSUA destruction. During quiescence, accept teardown callbacks but reject new incoming admission.

- [ ] Run the callback router test and require `PjsuaCallbackRouterTest PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaCallbackRouter.* voip/tests/pjsua/PjsuaCallbackRouterTest.cpp
  git commit -m "feat(voip): route pjsua callbacks to owned managers"
  ```

## Task 3: Add bounded account contexts and transactional creation

**Files:**

- Create: `voip/src/pjsua/PjsuaAccountContext.hpp`
- Create: `voip/src/pjsua/PjsuaAccountManager.hpp`
- Create: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Create: `voip/tests/pjsua/PjsuaAccountManagerTest.cpp`

**Interfaces:**

- Consumes: five `AgentRegistry` records and PJSUA account APIs.
- Produces: `pjsua_acc_id <-> AgentHandle` mapping, registration snapshots, five fixed retry records.

- [ ] Write fake-API tests for one/five account creation, PJSUA IDs returned out of order, correct user-data mapping, sixth rejection, duplicate/unknown ID callback rejection, and rollback when account 3 of 5 fails.

- [ ] Add an ownership test proving `PjsuaAccountContext` contains no `PcmSource*`, `PcmSink*`, or copied SIP credential arrays.

- [ ] Confirm tests fail because the manager is absent.

- [ ] Define:

  ```cpp
  struct PjsuaAccountContext {
      pjsua_acc_id account_id{PJSUA_INVALID_ID};
      AgentHandle agent{};
      RegistrationState registration{RegistrationState::disabled};
      RetryState retry{};
      bool occupied{};
  };
  ```

- [ ] Convert owned agent strings into temporary `pjsua_acc_config` views during actor-owned creation. Set identity, registrar, one Digest credential, `register_on_acc_add = PJ_FALSE`, TCP transport preference, disabled STUN for SIP/media, ICE off, and `use_srtp = PJSUA_SRTP_DISABLED`.

- [ ] Add all accounts with `pjsua_acc_add(&cfg, PJ_FALSE, &id)`, set context through `pjsua_acc_set_user_data(id, &context)`, and maintain a fixed ID-to-context lookup that does not assume ID equals config index.

- [ ] If any add fails, disable registration, delete previously added accounts in reverse order, clear user data, erase temporary credential buffers, leave zero occupied contexts, and return the mapped initialization error.

- [ ] Start `register_on_start` only after all five accounts, transport, callback router, event queue, and actor admission state are ready.

- [ ] Run `PjsuaAccountManagerTest` and require all transactional cases pass.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaAccountContext.hpp voip/src/pjsua/PjsuaAccountManager.* voip/tests/pjsua/PjsuaAccountManagerTest.cpp
  git commit -m "feat(voip): add transactional pjsua accounts"
  ```

## Task 4: Map registration callbacks and retry policy

**Files:**

- Modify: `voip/src/pjsua/PjsuaAccountManager.hpp`
- Modify: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Create: `voip/tests/pjsua/PjsuaRegistrationStateTest.cpp`

**Interfaces:**

- Consumes: `pjsua_reg_info`, actor clock/timer service, event reservations.
- Produces: independent `AgentSnapshot` transitions and bounded retry decisions.

- [ ] Write table-driven callback tests mapping provisional/success, 401 challenge progress, 403 authentication failure, 408/transport loss, unregister completion, and refresh success into public registration states/status.

- [ ] Add retry tests expecting delays `1,2,4,8,16,30,30` seconds plus deterministic per-agent bounded jitter, reset after success, and no automatic retry after authentication failure.

- [ ] Confirm tests fail against the unimplemented callback handler.

- [ ] In `OnRegistrationState(pjsua_acc_id, pjsua_reg_info*)`, copy the full public snapshot before publishing. Never retain the callback's `pjsip_rx_data`, `pj_str_t`, or temporary pointer.

- [ ] Classify 2xx as registered; 401/407 while PJSUA is authenticating as registering; final 401/403 as `authentication_failed`; network/timeout/5xx as recoverable `transport_failed` unless shutting down; explicit unregistration success as disabled.

- [ ] Implement retry state with attempt count, due timestamp, and per-agent deterministic jitter derived from handle slot. On due time call `pjsua_acc_set_registration(id, PJ_TRUE)` from the actor.

- [ ] Publish guaranteed registration-failure events and coalescible intermediate registration events. One agent's state must not mutate another's retry record.

- [ ] Run `PjsuaRegistrationStateTest` and require success.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua/PjsuaAccountManager.* voip/tests/pjsua/PjsuaRegistrationStateTest.cpp
  git commit -m "feat(voip): add independent registration state and retry"
  ```

## Task 5: Connect account management to `VoipRuntime`

**Files:**

- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/VoipRuntime.hpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Create: `voip/src/pjsua/PjsuaRuntimeAdapter.hpp`
- Create: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Modify: `voip/src/VoipService.cpp`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`
- Create: `voip/tests/integration/RegistrationIntegrationTest.cpp`

**Interfaces:**

- Consumes: core adapter seam and PJSUA account manager.
- Produces: production service initialization and agent registration events.

- [ ] Write an integration test with fake PJSUA that initializes five agents, checks handles map by configuration order despite scrambled PJSUA IDs, drives four successful registrations and one 403, and consumes independent polling events.

- [ ] Confirm failure before composing `PjsuaRuntimeAdapter`.

- [ ] Have the adapter initialize PJSUA, transport, callback router, then accounts transactionally. Only after success publish the initialized service state and start requested registrations.

- [ ] In each actor iteration, drain core commands/timers, invoke `pjsua_handle_events()` with a bounded timeout based on the next core deadline, then process resulting manager notifications before publishing snapshots.

- [ ] On initialization failure, tear down all PJ state before erasing registry credentials. Do not expose partially valid agent handles.

- [ ] Add `CONFIG_VOIP_PJSUA` dependencies on Plan 1 PJSUA/TCP gates and explicit conflicts with old `VOIP_PJ_BACKEND`. Keep `CONFIG_VOIP_SIP_TLS=n` and `CONFIG_VOIP_SRTP=n` in the initial product profile.

- [ ] Run all Plan 3 host/fake integration tests and require success.

- [ ] Commit:

  ```sh
  git add voip
  git commit -m "feat(voip): integrate pjsua account registration"
  ```

## Task 6: Validate five independent accounts over SIP TCP

**Files:**

- Create: `applications/voip_integration/src/multi_account_registration.cpp`
- Create: `applications/voip_integration/multi_account_registration.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Create: `applications/voip_integration/test_support/ScriptedRegistrar.hpp`
- Create: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`

**Interfaces:**

- Consumes: public VoIP polling API and a localhost scripted TCP registrar.
- Produces: QEMU proof of five independent Digest registration lifecycles.

- [ ] Implement a bounded scripted registrar accepting five TCP registrations, issuing Digest challenges with distinct realms/nonces, validating usernames, and returning configurable final status per identity. Do not log passwords or Authorization response contents.

- [ ] Write the integration scenario first: four users register; the fifth receives 403; the application consumes four registered and one authentication-failed snapshots; then server disconnect/recovery is applied to two users without changing the other three.

- [ ] Build before production wiring and confirm failure at service/account initialization.

- [ ] Build and run:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d build_voip_multi_account -- \
    -DEXTRA_CONF_FILE=multi_account_registration.conf
  west build -d build_voip_multi_account -t run
  ```

  Expected terminator: `VOIP MULTI ACCOUNT RESULT: PASSED (5 independent accounts)`.

- [ ] Repeat the entire initialize/register/shutdown lifecycle five times. After each cycle require zero PJSUA accounts, no active transport, no live PJ arena blocks after destruction, and erased credential storage.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): validate five PJSUA registrations"
  ```

## Plan 3 Exit Criteria

- One actor owns one PJSUA runtime and one plain SIP TCP transport.
- One to five accounts are added transactionally only at initialization.
- PJSUA account IDs map through `PjsuaAccountContext` to stable agent handles.
- Account contexts do not own audio bindings.
- Five registration lifecycles, retry timers, failures, and events are independent.
- Authentication failures do not retry; recoverable failures use bounded backoff.
- TLS and SRTP remain visible policy options but disabled and unused.
