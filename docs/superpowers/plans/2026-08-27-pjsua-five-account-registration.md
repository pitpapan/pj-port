# PJSUA Five-Account Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Each task must be implemented by a Terra-medium worker in an isolated worktree, then reviewed and validated by the primary agent before the next task starts.

**Goal:** Replace the Plan 2 placeholder adapter with one actor-owned PJSUA-LIB runtime that transactionally creates one to five initialization-time SIP accounts and reports five independent registration lifecycles over plain SIP TCP.

**Architecture:** `VoipRuntime` starts its actor before native initialization and performs a synchronous startup handshake. The actor composes `PjsuaRuntimeAdapter`, which owns `PjsuaRuntime`, `PjsuaTransportManager`, `PjsuaCallbackRouter`, and `PjsuaAccountManager`. PJSUA has zero workers and is pumped by the existing polling actor. Stable `PjsuaAccountContext[5]` records map native account IDs to `AgentHandle`; SIP strings, credentials, and audio bindings remain owned by `AgentRegistry`.

**Tech Stack:** C++17, PJSUA-LIB C API, PJPROJECT's fixed Zephyr pool arena, SIP over TCP, Digest authentication, fixed arrays/rings, Zephyr actor thread, injectable PJSUA function table, host tests, and `mps2/an385` QEMU integration tests.

**Spec:** `docs/superpowers/specs/2026-08-27-multi-agent-pjsua-voip-architecture-design.md`

**Baseline:** Plan 1 and Plan 2 are merged at or after `688de098f`. Plan 1 already ports PJSUA-LIB, fixes capacities at 5 accounts/7 calls/12 conference ports, and supplies the bounded arena. Plan 2 already supplies the public service, immutable five-agent registry, polling event queue, actor, operations, business state machine, two promoted slots, and five-entry strict FIFO.

## Global Constraints

- Never inspect, search, index, or modify top-level `zephyr/`.
- Use PJSUA-LIB's C API; do not use PJSUA2 or implement a parallel low-level registration client.
- Configure one to five agents only during `Initialize()`; topology, SIP credentials, and audio bindings remain immutable until shutdown.
- Keep exactly one PJSUA runtime and one plain SIP TCP transport per initialized production service.
- Set PJSUA and PJMEDIA worker counts to zero; only the core actor calls `pjsua_handle_events()`.
- Do not use the general heap after successful initialization. PJ allocations must remain inside the Plan 1 arena.
- Do not enable TLS or SRTP. Keep their policy/Kconfig boundaries visible and reject enabled policies before PJSUA is created.
- Do not choose or reprioritize codecs in this plan.
- Do not put `PcmSource`, `PcmSink`, or copied SIP credentials in `PjsuaAccountContext`.
- Do not implement PJSUA call control or media here. Until Plan 4, adapter call methods return `unsupported_configuration` (an already-admitted asynchronous public operation completes with that error), and unexpected incoming INVITEs are rejected without allocating a logical call.
- Keep the legacy lowercase `PjVoipBackend` and uppercase `modules/VOIP` stack buildable; removal is a later migration task.
- Preserve Plan 2's public business state machine, two-global/one-per-agent promotion policy, and five-entry strict FIFO unchanged.
- Run `git diff --check`, relevant tests, and the task's review checkpoint before every commit.

## Execution and Review Protocol

For every task:

1. The primary agent creates or reuses an isolated Plan 3 worktree.
2. A Terra-medium subagent implements only that task using test-first steps.
3. The primary agent reviews the diff against this plan, `designIdead.md`, `reviews.md`, and the approved architecture.
4. The primary agent runs the task's validation commands independently.
5. Findings return to the same Terra-medium worker; the task is not accepted until the review is clean.
6. Commit only the reviewed task, then continue to the next task.

---

## Task 1: Make native startup actor-owned and transactional

**Files:**

- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.cpp`
- Modify: `voip/src/core/VoipRuntime.hpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/core/CoreActor.hpp`
- Modify: `voip/src/core/CoreActor.cpp`
- Modify: `voip/tests/unit/VoipServiceCoreTest.cpp`
- Modify: `voip/tests/run_host_tests.sh`

**Interface change:** Replace per-agent, caller-thread initialization with one actor-owned lifecycle:

```cpp
class RuntimeAdapter {
public:
    virtual Error Initialize(const AgentRegistry &, const SecurityPolicy &,
                             const PcmFormat &) noexcept = 0;
    virtual Error Pump(std::uint64_t now_ms,
                       std::uint32_t timeout_ms) noexcept = 0;
    virtual bool TryGetNotification(RuntimeNotification *) noexcept = 0;
    // Existing call operations remain.
    virtual Error Shutdown() noexcept = 0;
};
```

`RuntimeNotification` gains a `registration_state` type plus copied `RegistrationState` and `Status` fields. Remove the misleading `agent_registered` notification and the `initialize_account` request.

- [ ] Add failing host tests for a synchronous startup handshake: `Initialize()` must not return success until the actor has initialized the adapter, all configured agents are visible, and initial agent events can be published.

- [ ] Add a failure-injection test where whole-adapter initialization fails after the registry copied five agents. Require `Initialize()` to return that error, stop/join the actor, clear events, erase registry credentials, expose no handles, and allow a later clean initialization.

- [ ] Add a fake-adapter test that emits independent registration notifications for five handles, including one `authentication_failed`, and assert `AgentSnapshot` plus `Event::status` are updated only for the matching agent.

- [ ] Add a regression test proving `Initialize()` does not wait while holding `VoipRuntime::mutex_`; the actor must be able to enter its bootstrap step and signal completion.

- [ ] Confirm RED by changing the tests first: the current `InitializeAccount()` loop runs before `CoreActor::Start()` and cannot satisfy the actor-owned contract.

- [ ] Replace the boolean startup flags with an explicit private lifecycle such as `idle`, `starting`, `running`, `shutting_down`, and `stopped`, plus `startup_signal_` and copied `startup_error_`.

- [ ] In public `VoipRuntime::Initialize()`, validate/copy the full `ServiceConfig`, reset fixed structures, set `starting`, and start the actor. Release `mutex_` before waiting for `startup_signal_`.

- [ ] In the actor's first `Step()`, call the one whole-runtime `adapter_.Initialize(agents_, config.security, config.conference_format)`. Publish initial `disabled`/`registering` snapshots only after that call succeeds; then mark the runtime running and signal the public initializer.

- [ ] On bootstrap failure, perform adapter rollback on the actor, signal the exact error, stop/join the actor from the public thread, then reset the registry and event lifecycle. Never publish partially valid handles.

- [ ] Change the normal actor iteration to call `adapter_.Pump(now_ms, 0)` and then drain at most `RuntimeAdapter::notification_capacity` records with `TryGetNotification()`. The zero timeout preserves the existing 1 ms polling loop and avoids blocking public APIs under the current Plan 2 mutex.

- [ ] Update `FakeRuntimeAdapter` to initialize all registry agents transactionally and to enqueue copied `registration_state` records. Keep all fixed capacities and all Plan 2 call-operation behavior.

- [ ] Make `ProcessNotification(registration_state)` resolve only `notification.agent`, copy the state into its `AgentContext`, and publish `EventType::agent_snapshot` with the copied status. Authentication/transport failures must retain the event queue's existing guaranteed-event behavior; intermediate changes remain coalescible.

- [ ] Run:

  ```sh
  voip/tests/run_host_tests.sh
  git diff --check
  ```

  Expected: every existing Plan 2 test and the new startup/registration tests pass.

- [ ] Commit:

  ```sh
  git add voip/src/core voip/tests
  git commit -m "refactor(voip): make runtime adapter startup actor-owned"
  ```

---

## Task 2: Add the native PJSUA runtime, TCP transport, and callback boundary

**Files:**

- Create: `voip/src/pjsua/PjsuaApi.hpp`
- Create: `voip/src/pjsua/PjsuaApi.cpp`
- Create: `voip/src/pjsua/PjsuaRuntime.hpp`
- Create: `voip/src/pjsua/PjsuaRuntime.cpp`
- Create: `voip/src/pjsua/PjsuaTransportManager.hpp`
- Create: `voip/src/pjsua/PjsuaTransportManager.cpp`
- Create: `voip/src/pjsua/SignalingTransportPolicy.hpp`
- Create: `voip/src/pjsua/PjsuaCallbackRouter.hpp`
- Create: `voip/src/pjsua/PjsuaCallbackRouter.cpp`
- Create: `voip/tests/pjsua/FakePjsuaApi.hpp`
- Create: `voip/tests/pjsua/PjsuaRuntimeTest.cpp`
- Create: `voip/tests/pjsua/PjsuaCallbackRouterTest.cpp`
- Create: `voip/tests/pjsua/PjsuaPlan3TestMain.cpp`
- Modify: `voip/src/core/AgentRegistry.cpp`
- Modify: `voip/tests/unit/AgentRegistryTest.cpp`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`
- Create: `applications/voip_integration/pjsua_registration_fake.conf`

**Component APIs:**

```cpp
class PjsuaRuntime {
public:
    Error CreateAndInitialize(const pjsua_callback &,
                              const PcmFormat &) noexcept;
    Error Start() noexcept;
    Error Pump(std::uint32_t timeout_ms) noexcept;
    Error Destroy() noexcept;
};

class PjsuaTransportManager {
public:
    Error Initialize(SignalingTransportPolicy) noexcept;
    pjsua_transport_id Id() const noexcept;
    Error Shutdown() noexcept;
};
```

`PjsuaApi` is a narrow injectable function table for only the PJSUA and Plan 1 arena calls used by the new components. Production uses `NativePjsuaApi()`; QEMU component tests inject deterministic statuses and callbacks.

`PjsuaCallbackRouter.hpp` also defines a private-to-the-adapter `PjsuaCallbackSink` and fixed copied registration records. The router's static C callbacks copy native data into those records and call the sink; Task 3 binds `PjsuaAccountManager` as that sink. This keeps the router testable without putting account policy or mutable registration state in the process-global trampoline.

- [ ] Write the fake-API tests first. Require exact successful ordering: arena install, `pjsua_create`, defaults, `pjsua_init`, no-sound selection, TCP transport create, `pjsua_start`, explicit event pumping, transport close, `pjsua_destroy`, arena reset.

- [ ] Add failure injection at arena install, create, init, no-sound selection, transport create, and start. Require rollback of only acquired stages in reverse order; repeated `Destroy()`/`Shutdown()` must be safe.

- [ ] Add callback-router tests for single attach, rejection of a second active router, forwarding to the owned instance, quiescence, and safe detachment after destruction.

- [ ] Confirm RED because `voip/src/pjsua/` does not exist.

- [ ] Make `PjsuaRuntime::CreateAndInitialize()` record the actor thread and configure:

  ```cpp
  ua.max_calls = 7;
  ua.thread_cnt = 0;
  ua.stun_srv_cnt = 0;
  ua.enable_upnp = PJ_FALSE;
  ua.use_srtp = PJMEDIA_SRTP_DISABLED;
  media.thread_cnt = 0;
  media.has_ioqueue = PJ_FALSE;
  media.max_media_ports = 12;
  ```

- [ ] Map the service conference format into `media.clock_rate`, `media.channel_count`, and integral `media.audio_frame_ptime`. Extend `AgentRegistry` validation only if needed so invalid frame durations fail before the arena is installed.

- [ ] Disable SIP-message logging in the initial runtime so Digest Authorization content and credentials are never printed. Do not add any password-bearing diagnostic record.

- [ ] Call `pjsua_set_no_snd_dev()` and require a non-null result. Do not create or select a platform sound device; Plan 5 will add custom audio ports.

- [ ] Implement only `SignalingTransportPolicy::tcp_plain`. Create exactly one `PJSIP_TRANSPORT_TCP` transport and retain its returned ID. Any TLS policy returns `unsupported_configuration` before native initialization.

- [ ] Add compile-time checks that `PJSUA_MAX_ACC == 5`, `PJSUA_MAX_CALLS == 7`, `PJSUA_MAX_CONF_PORTS == 12`, `PJSIP_HAS_TLS_TRANSPORT == 0`, and `PJMEDIA_HAS_SRTP == 0` in the initial profile.

- [ ] Have `PjsuaCallbackRouter` own the one process-global trampoline pointer but no domain state. For Plan 3 populate `on_reg_started2`, `on_reg_state2`, and a temporary incoming-call guard. The guard rejects unexpected INVITEs with `486 Busy Here` and releases the native call; Plan 4 replaces it with scheduler routing.

- [ ] `BeginQuiescence()` must reject new incoming work while still forwarding registration teardown callbacks. `Detach()` is legal only after PJSUA destruction. All instance entry points assert actor-thread ownership in debug builds.

- [ ] Build and run the component-test profile:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d /tmp/voip-plan3-pjsua-fake -- \
    -DEXTRA_CONF_FILE=pjsua_registration_fake.conf
  west build -d /tmp/voip-plan3-pjsua-fake -t run
  ```

  Expected terminator: `PJSUA PLAN 3 COMPONENT RESULT: PASSED`.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua applications/voip_integration
  git commit -m "feat(voip): add actor-owned pjsua runtime boundary"
  ```

---

## Task 3: Create five bounded PJSUA account contexts transactionally

**Files:**

- Create: `voip/src/pjsua/PjsuaAccountContext.hpp`
- Create: `voip/src/pjsua/PjsuaAccountManager.hpp`
- Create: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Create: `voip/tests/pjsua/PjsuaAccountManagerTest.cpp`
- Modify: `voip/tests/pjsua/PjsuaPlan3TestMain.cpp`

**Owned context:**

```cpp
struct RetryState {
    std::uint8_t attempt{};
    std::uint64_t due_ms{};
    bool scheduled{};
};

struct PjsuaAccountContext {
    pjsua_acc_id account_id{PJSUA_INVALID_ID};
    AgentHandle agent{};
    RegistrationState registration{RegistrationState::disabled};
    RetryState retry{};
    std::uint64_t refresh_due_ms{};
    bool register_on_start{};
    bool occupied{};
};
```

`PjsuaAccountManager` must also expose bounded Plan 4 prerequisites: `Resolve(pjsua_acc_id)` and `NativeId(AgentHandle)`. Neither lookup transfers ownership or exposes PJPROJECT types outside `voip/src/pjsua/`.

- [ ] Write fake-API tests for one account, five accounts, native IDs returned in a scrambled order, sixth-account rejection, duplicate/unknown native ID rejection, and rollback when account 3 of 5 fails.

- [ ] Add tests that inspect captured `pjsua_acc_config` values for all five identities, registrar URIs, usernames, passwords, TCP transport ID, disabled NAT/security features, disabled native retry, and `register_on_acc_add == PJ_FALSE`.

- [ ] Add compile-time ownership checks and code-review assertions that `PjsuaAccountContext` has no `PcmSource*`, `PcmSink*`, `OwnedSipAccountConfig`, username array, or password array.

- [ ] Confirm RED because the account manager is absent.

- [ ] Give `PjsuaAccountManager` exactly five stable context slots and a fixed native-ID lookup. Never assume `pjsua_acc_id` equals configuration index; map through PJSUA user data and validate the ID before dereference.

- [ ] Build each `pjsua_acc_config` from `pjsua_acc_config_default()`. Set `cfg.user_data = &context` before `pjsua_acc_add()` so no callback window exists without a stable context.

- [ ] Borrow the registry's stable copied strings only while filling the native configuration. Set identity, registrar, one Digest/plain-password credential, the shared TCP transport ID, `register_on_acc_add = PJ_FALSE`, SIP/media STUN disabled, SIP/media UPnP disabled, custom ICE/TURN disabled, and `use_srtp = PJMEDIA_SRTP_DISABLED`.

- [ ] Disable PJSUA's built-in registration retry by setting `reg_retry_interval`, `reg_first_retry_interval`, and `reg_retry_random_interval` to zero. Plan 3 owns the required exponential schedule.

- [ ] Add every configured account before starting any registration. If an add fails, clear user data, delete previously added accounts in reverse order, clear all lookup/context slots, and return the mapped initialization error.

- [ ] Do not treat a server registration failure as initialization failure. After all accounts exist and the actor/event queue are ready, request registration only for contexts whose registry record has `register_on_start == true`; disabled agents remain disabled.

- [ ] Extend the component profile and require `PjsuaAccountManagerTest PASSED` plus the overall component terminator.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua
  git commit -m "feat(voip): add transactional pjsua account manager"
  ```

---

## Task 4: Implement independent registration state, refresh, and retry

**Files:**

- Modify: `voip/src/pjsua/PjsuaCallbackRouter.hpp`
- Modify: `voip/src/pjsua/PjsuaCallbackRouter.cpp`
- Modify: `voip/src/pjsua/PjsuaAccountManager.hpp`
- Modify: `voip/src/pjsua/PjsuaAccountManager.cpp`
- Create: `voip/tests/pjsua/PjsuaRegistrationStateTest.cpp`
- Modify: `voip/tests/pjsua/PjsuaPlan3TestMain.cpp`

**Callback boundary:** The router must immediately copy `status`, SIP code, reason text, renewal/unregistration flags, and expiration from `pjsua_reg_info`/`pjsip_regc_cbparam`. It must never retain `pjsua_reg_info`, `pjsip_regc_cbparam`, `pjsip_rx_data`, `pj_str_t`, or their pointers.

- [ ] Write table-driven tests for registration start, success, refresh success, explicit unregistration success, final 401/407/403, 408, transport/PJ error, recoverable 5xx/6xx, and permanent non-authentication 4xx.

- [ ] Write exact retry tests for attempts at base delays `1, 2, 4, 8, 16, 30, 30` seconds. Define deterministic jitter as `agent.slot * 50 ms`, yielding 0-200 ms for five agents, and assert each due timestamp exactly.

- [ ] Add isolation tests: failure/retry for agents 1 and 3 must not change retry counters, due times, snapshots, or native registration calls for agents 0, 2, and 4.

- [ ] Add tests that success resets retry attempt/due state, authentication failure never retries, a disabled account never registers, and an adapter notification contains only copied public data.

- [ ] Confirm RED against the account manager from Task 3.

- [ ] On `on_reg_started2`, publish `registering` for initial renewal and `unregistering` for explicit unregistration. Publish `refreshing` from the account timer immediately before PJSUA's expected automatic refresh boundary.

- [ ] On successful renewal, publish `registered`, reset retry state, and schedule the next refresh projection from the returned expiration. Configure `reg_delay_before_refresh = 1` second so the QEMU registrar can use short deterministic expirations.

- [ ] Treat final 401, 407, and 403 as `authentication_failed` with no retry. Treat PJ transport errors, 408, 480, 5xx, and 6xx as `transport_failed`, then schedule `retry_wait`. Preserve the exact SIP code, mapped `Error`, and bounded copied reason.

- [ ] For permanent non-authentication 4xx, publish `transport_failed` with the exact status but do not schedule an automatic retry. The public state model intentionally has no second generic failure enum.

- [ ] On a retry deadline, call `pjsua_acc_set_registration(id, PJ_TRUE)` from the actor, publish `registering`, and advance only that context's retry attempt. An immediate native-call failure re-enters the same bounded schedule.

- [ ] Ensure failure notifications reach `VoipRuntime` before `retry_wait`, so the guaranteed failure event cannot be coalesced away by the later intermediate snapshot.

- [ ] Extend the component profile and require `PjsuaRegistrationStateTest PASSED` plus the overall component terminator.

- [ ] Commit:

  ```sh
  git add voip/src/pjsua voip/tests/pjsua
  git commit -m "feat(voip): add independent pjsua registration lifecycle"
  ```

---

## Task 5: Compose `PjsuaRuntimeAdapter` and complete ordered shutdown

**Files:**

- Create: `voip/src/pjsua/PjsuaRuntimeAdapter.hpp`
- Create: `voip/src/pjsua/PjsuaRuntimeAdapter.cpp`
- Modify: `voip/src/core/VoipRuntime.hpp`
- Modify: `voip/src/core/VoipRuntime.cpp`
- Modify: `voip/src/core/RuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.hpp`
- Modify: `voip/src/core/FakeRuntimeAdapter.cpp`
- Modify: `voip/src/VoipService.cpp`
- Modify: `voip/zephyr/Kconfig`
- Modify: `voip/zephyr/CMakeLists.txt`
- Modify: `applications/voip_integration/Kconfig`
- Create: `voip/tests/pjsua/PjsuaRuntimeAdapterTest.cpp`
- Modify: `voip/tests/pjsua/PjsuaPlan3TestMain.cpp`

**Initialization order:**

```text
validate/copy ServiceConfig in AgentRegistry
start actor
attach callback router
install arena and create/init PJSUA
create the one TCP transport
start PJSUA
add all accounts with registration disabled
mark adapter/event routing ready
request register_on_start accounts
signal successful public Initialize
```

- [ ] Write an adapter integration test that initializes five registry records, receives scrambled native account IDs, requests four registrations plus one disabled account, pumps callbacks, and returns independent copied notifications.

- [ ] Add failure tests for every orchestration boundary: router attach, runtime init, transport create, runtime start, each account add, and initial registration request. Failures through account creation roll back the entire startup; a registration-request/server failure becomes an agent event without destroying other accounts.

- [ ] Add a second-service test: while one production router/runtime is attached, a second `VoipService::Initialize()` must return `invalid_state` without changing the first service.

- [ ] Add a registration-only call guard test: an incoming INVITE receives 486, no logical-call notification is emitted, and no native call remains. Every adapter outgoing/call-control method returns `unsupported_configuration` until Plan 4; any public operation already accepted asynchronously receives one terminal completion carrying that error.

- [ ] Add shutdown tests from disabled, registering, registered, retry-wait, authentication-failed, and transport-failed states. Require this order: begin router quiescence, request unregistration where applicable, keep pumping callbacks, clear account user data, delete accounts, close transport, destroy PJSUA, reset arena, detach router, then allow credential erasure/handle invalidation in core.

- [ ] Confirm RED before creating `PjsuaRuntimeAdapter` and selecting it.

- [ ] Compose the four focused PJSUA components without moving their responsibilities into the adapter. The adapter translates core lifecycle/call methods and owns a fixed 32-record notification ring; it contains no SIP parser, registrar logic, audio binding, or scheduler policy.

- [ ] Implement `Shutdown()` as an idempotent actor-driven state machine. Return `Error::busy` while unregistration callbacks are pending so `VoipRuntime::Step()` continues pumping. Return `ok` only after PJSUA destruction and arena reset.

- [ ] Preserve current public `shutdown_timeout` behavior: if the public wait expires, return `shutdown_timeout` while the actor and all callback-reachable storage remain alive. A later `Shutdown()` resumes the same teardown; never force-free reachable contexts.

- [ ] Select adapters explicitly:

  ```cpp
  #if defined(CONFIG_VOIP_PJSUA)
  using SelectedRuntimeAdapter = PjsuaRuntimeAdapter;
  #elif !defined(__ZEPHYR__) || defined(CONFIG_VOIP_SERVICE_FAKE_ADAPTER)
  using SelectedRuntimeAdapter = FakeRuntimeAdapter;
  #else
  using SelectedRuntimeAdapter = NullRuntimeAdapter;
  #endif
  ```

- [ ] Update `VoipService`'s placement-storage assertion after composition. Do not silently increase its 128 KiB fixed C++ storage unless the measured `sizeof(Impl)` requires a reviewed bounded change.

- [ ] Make `CONFIG_VOIP_PJSUA` depend on `PJSUA` and `PJSIP_TCP_TRANSPORT`. Move the reserved `VOIP_SIP_TLS` and `VOIP_SRTP` symbols into the VoIP module if necessary so applications do not redefine them; both remain default `n` and unsupported.

- [ ] Add only `src/pjsua/*.cpp` production sources under `CONFIG_VOIP_PJSUA`. Keep the fake adapter and legacy backend mutually exclusive with the PJSUA service adapter.

- [ ] Run:

  ```sh
  voip/tests/run_host_tests.sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d /tmp/voip-plan3-pjsua-fake -- \
    -DEXTRA_CONF_FILE=pjsua_registration_fake.conf
  west build -d /tmp/voip-plan3-pjsua-fake -t run
  git diff --check
  ```

- [ ] Commit:

  ```sh
  git add voip applications/voip_integration
  git commit -m "feat(voip): integrate pjsua registration adapter"
  ```

---

## Task 6: Prove five independent Digest registrations over SIP TCP

**Files:**

- Create: `applications/voip_integration/test_support/ScriptedRegistrar.hpp`
- Create: `applications/voip_integration/test_support/ScriptedRegistrar.cpp`
- Create: `applications/voip_integration/src/pjsua_registration.cpp`
- Create: `applications/voip_integration/pjsua_registration.conf`
- Modify: `applications/voip_integration/Kconfig`
- Modify: `applications/voip_integration/CMakeLists.txt`

**Test topology:** Use a test-only PJSIP registrar module on the PJSUA endpoint's loopback TCP listener, adapting the proven Digest/refresh machinery in `applications/voip_integration/src/phase4_registration.cpp`. Production components must not expose a native endpoint in their public API.

- [ ] Write the integration scenario before completing registrar behavior. Construct five distinct `PcmSource`/`PcmSink` stubs because audio bindings are mandatory, but assert that registration never invokes them.

- [ ] Configure five agents only in `ServiceConfig::agents`: four valid credentials and one deliberately wrong password. Keep all input strings temporary and prove they can be overwritten after `Initialize()` returns because `AgentRegistry` copied them.

- [ ] Make the registrar challenge each identity with deterministic Digest data, validate the username/password pair, return 200 to four agents, and return final 403 to the bad credential. Never print a password, Authorization header, or Digest response.

- [ ] Consume only the public polling API. Require four independent `registered` snapshots and one guaranteed `authentication_failed` event with SIP 403; verify handles map in configuration order. The scrambled native-ID proof remains in the fake component test where IDs can be controlled deterministically.

- [ ] Script one 408 response for agents 1 and 3. Require `transport_failed`, `retry_wait`, and later `registered` for only those agents at their deterministic retry deadlines. Agents 0, 2, and 4 must retain their prior states and receive no false failure event.

- [ ] Use a short registration expiration to observe automatic refresh for at least two accounts. Require `refreshing -> registered` without altering another account's retry state.

- [ ] Verify every REGISTER reaches the registrar through TCP, PJSUA/PJMEDIA worker counts remain zero, and only the core actor drives progress.

- [ ] Run one shutdown while retries are pending. Drain events through `service_stopped` and prove it is final. Then repeat the complete initialize/register/fail/recover/shutdown lifecycle five times.

- [ ] After every lifecycle require: no native accounts, no transport, detached callback router, `pjsua_get_state() == PJSUA_STATE_NULL`, zero live Plan 1 arena blocks, zero used arena bytes, and invalidated public handles. Credential buffers are reviewed/checked as erased after shutdown.

- [ ] Build and run:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d /tmp/voip-plan3-registration -- \
    -DEXTRA_CONF_FILE=pjsua_registration.conf
  west build -d /tmp/voip-plan3-registration -t run
  ```

  Expected terminator: `VOIP PJSUA REGISTRATION RESULT: PASSED (5 independent accounts, 5 lifecycles)`.

- [ ] Commit:

  ```sh
  git add applications/voip_integration
  git commit -m "test(voip): validate five pjsua registrations"
  ```

---

## Task 7: Run the Plan 3 regression gate and record acceptance

**Files:**

- Create: `docs/voip/pjsua-five-account-registration.md`
- Modify only if findings require it: Plan 3 production/test files

- [ ] Run the complete host core suite twice to catch startup/shutdown races:

  ```sh
  voip/tests/run_host_tests.sh
  voip/tests/run_host_tests.sh
  ```

- [ ] Rebuild/run the Plan 1 PJSUA capacity profile to prove Plan 3 did not regress the port boundary:

  ```sh
  west build -p always -b mps2/an385 applications/voip_integration \
    -d /tmp/voip-plan3-capacity -- \
    -DEXTRA_CONF_FILE=pjsua_capacity.conf
  west build -d /tmp/voip-plan3-capacity -t run
  ```

  Expected terminator: `PJSUA CAPACITY RESULT: PASSED (5 accounts, 7 calls, eighth 486)`.

- [ ] Rebuild/run both Plan 3 profiles and capture their exact pass markers, PJSUA worker-thread evidence, arena peak/cleanup values, and repeated-lifecycle results in the acceptance record.

- [ ] Add negative public-service tests for `SignalingSecurity::tls` and `MediaSecurity::srtp_sdes`. Require `unsupported_configuration` before router attach, arena install, PJSUA create, account creation, or credential-bearing logging.

- [ ] Run a no-heap-after-initialization scenario that registers, refreshes, fails, retries, polls events, and shuts down while the Plan 1 arena accounts for all PJ blocks. Any general-heap use after successful `Initialize()` is a release blocker.

- [ ] Review the complete diff against:

  - `designIdead.md`: configurable agent identities, independent registration, per-agent audio ownership, two global calls, queueing.
  - `reviews.md`: split the monolithic backend, use PJPROJECT rather than rebuilding SIP, keep real audio/multi-call gaps honest.
  - `state_machine.uml`: no call-state transition or queue-policy change in Plan 3.
  - Approved architecture sections 7.7, 7.8, 7.11, 15, 17, 18, 22, and 23.

- [ ] Confirm Plan 3 contains no call manager, media bridge, codec-priority policy, TLS implementation, SRTP implementation, PJSUA2 use, runtime account mutation, or legacy-stack deletion.

- [ ] Run final repository checks:

  ```sh
  git diff --check
  git status --short
  ```

- [ ] Commit:

  ```sh
  git add docs/voip/pjsua-five-account-registration.md
  git commit -m "docs(voip): record five-account registration acceptance"
  ```

## Plan 3 Exit Criteria

- Public `Initialize()` performs one synchronous actor-owned native startup and exposes no partial handles on failure.
- Exactly one PJSUA runtime, one callback router, one plain TCP transport, and one to five PJSUA accounts exist per initialized service.
- All accounts are created transactionally before any requested registration starts.
- Native account IDs map through stable `PjsuaAccountContext` records to configuration-order `AgentHandle` values.
- The account manager exposes the bounded native-ID/agent lookup needed by Plan 4 without leaking PJPROJECT into public headers.
- `PjsuaAccountContext` owns no credentials or audio binding; `AgentRegistry` remains their sole product owner.
- Five registration, refresh, failure, retry, and unregistration lifecycles remain independent.
- Authentication failure never retries; recoverable failures use the exact bounded exponential schedule plus deterministic per-agent jitter.
- PJSUA callbacks copy all public data before returning and never invoke application code.
- PJSUA and PJMEDIA use zero workers; the existing actor polling loop exclusively calls `pjsua_handle_events()`.
- Shutdown unregisters/deletes accounts, closes transport, destroys PJSUA, resets the arena, detaches callbacks, erases credentials, invalidates handles, and publishes `service_stopped` last.
- TLS and SRTP policy corners remain visible but disabled and unused; codecs, calls, and media remain explicitly deferred.
- Host tests, PJSUA component tests, five-account QEMU integration, repeated lifecycle checks, and Plan 1 capacity regression all pass.
