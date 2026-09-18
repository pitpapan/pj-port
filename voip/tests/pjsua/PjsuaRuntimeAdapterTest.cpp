#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"

#include <cassert>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source sources[5]{}; Sink sinks[5]{}; AgentConfig agents[5]{};
    ServiceConfig config{agents, 5, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    Fixture() noexcept { for (std::size_t i = 0; i < 5; ++i) agents[i] = {{i == 0 ? "sip:zero@example.test" : i == 1 ? "sip:one@example.test" : i == 2 ? "sip:two@example.test" : i == 3 ? "sip:three@example.test" : "sip:four@example.test", "sip:registrar.example.test", "user", "password"}, {&sources[i], &sinks[i]}, i != 4}; }
};
}

void RunPjsuaRuntimeAdapterTests() {
    constexpr unsigned adapter_limit = 7;
    Fixture fixture; AgentRegistry registry; assert(registry.Initialize(fixture.config) == Error::ok);
    static FakePjsuaApi fake; fake.Activate(); PjsuaRuntimeAdapter adapter(fake.Api());
    auto initial_lifecycle = [&]() __attribute__((noinline)) {
    assert(adapter.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::ok);
    assert(fake.AccountAddCount() == 5 && fake.RegistrationCount() == 4);
    assert(adapter.Pump(100, 0) == Error::ok);
    RuntimeNotification notification{};
    assert(!adapter.TryGetNotification(&notification));
    assert(adapter.PromoteOutgoing({}, "sip:peer@example.test", nullptr) == Error::unsupported_configuration);
    assert(adapter.PromoteIncoming({}, 1, nullptr) == Error::unsupported_configuration);
    assert(adapter.Answer(1) == Error::unsupported_configuration);
    assert(adapter.Reject(1, 486) == Error::unsupported_configuration);
    assert(adapter.Cancel(1) == Error::unsupported_configuration);
    assert(adapter.Hangup(1) == Error::unsupported_configuration);
    assert(adapter.SetHeld(1, true) == Error::unsupported_configuration);
    assert(adapter.SetHeld(1, false) == Error::unsupported_configuration);
    assert(adapter.BeginCallTeardown(1) == Error::unsupported_configuration);
    assert(fake.AnswerCount() == 0 && fake.HangupCount() == 0);
    fake.SetUnregistrationCallbacksDeferred(true);
    assert(adapter.Shutdown() == Error::busy);
    assert(adapter.Pump(101, 0) == Error::ok);
    assert(adapter.Shutdown() == Error::busy);
    // The adapter must retain every callback-reachable account context while
    // unregister completion is pending; elapsed pump time is not permission
    // to tear it down.
    assert(adapter.Pump(1100, 0) == Error::ok);
    assert(adapter.Shutdown() == Error::busy);
    fake.DeliverUnregistrationCallbacks();
    assert(adapter.Pump(1101, 0) == Error::ok);
    assert(adapter.Shutdown() == Error::ok);
    printk("PjsuaRuntimeAdapterTest first lifecycle\n");
    assert(fake.SequenceEquals("arena,create,defaults,init,nosnd,tcp,start,add,add,add,add,add,reg,reg,reg,reg,pump,reg,reg,reg,reg,pump,pump,pump,clear,del,clear,del,clear,del,clear,del,clear,del,close,destroy,reset"));
    };
    if (adapter_limit >= 1) initial_lifecycle();

    auto duplicate_adapter = [&]() __attribute__((noinline)) {
    fake.Reset(); PjsuaRuntimeAdapter first(fake.Api());
    PjsuaRuntimeAdapter second(fake.Api());
    assert(first.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::ok);
    assert(second.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::invalid_state);
    assert(first.Shutdown() == Error::busy); (void)first.Pump(1, 0); (void)first.Shutdown(); (void)first.Pump(1001, 0); assert(first.Shutdown() == Error::ok);
    };
    if (adapter_limit >= 2) duplicate_adapter();

    auto unregistration_failure = [&]() __attribute__((noinline)) {
    fake.Reset();
    PjsuaRuntimeAdapter failed_unregistration(fake.Api());
    const unsigned callbacks_before = PjsuaCallbackRouter::CallbackEntryCountForTest();
    assert(failed_unregistration.Initialize(registry, fixture.config.security,
                                            fixture.config.conference_format) == Error::ok);
    fake.FailUnregistration(PJ_EUNKNOWN);
    assert(failed_unregistration.Shutdown() == Error::busy);
    // An immediate native failure has no callback to wait for.  Teardown must
    // continue on the next actor step instead of relying on a timeout.
    assert(failed_unregistration.Shutdown() == Error::ok);
    assert(fake.AccountAddCount() == 5 && fake.RegistrationCount() == 8 &&
           fake.AccountClearCount() == 5 && fake.AccountDeleteCount() == 5);
    assert(PjsuaCallbackRouter::CallbackEntryCountForTest() == callbacks_before);
    assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);
    };
    if (adapter_limit >= 3) unregistration_failure();

    auto startup_transaction = [&]() __attribute__((noinline)) {
    // Startup is transactional at every composition boundary.  These calls
    // deliberately use test-only fake controls added with this matrix.
    for (FakePjsuaApi::Stage stage : {FakePjsuaApi::Stage::arena,
                                      FakePjsuaApi::Stage::create,
                                      FakePjsuaApi::Stage::init,
                                      FakePjsuaApi::Stage::no_sound,
                                      FakePjsuaApi::Stage::transport,
                                      FakePjsuaApi::Stage::start}) {
        fake.Reset(); fake.Fail(stage);
        PjsuaRuntimeAdapter candidate(fake.Api());
        assert(candidate.Initialize(registry, fixture.config.security,
                                    fixture.config.conference_format) != Error::ok);
        assert(fake.AccountAddCount() == 0);
    }
    for (std::size_t position = 1; position <= 5; ++position) {
        fake.Reset(); fake.FailAccountAdd(position);
        PjsuaRuntimeAdapter candidate(fake.Api());
        assert(candidate.Initialize(registry, fixture.config.security,
                                    fixture.config.conference_format) != Error::ok);
        assert(fake.AccountAddCount() == position);
        assert(fake.AccountDeleteCount() == position - 1);
    }
    };
    if (adapter_limit >= 4) startup_transaction();

    auto deferred_startup_cleanup = [&]() __attribute__((noinline)) {
        // If PJLIB TLS registration fails while startup is unwinding, the
        // adapter must retain every native object until an actor turn can
        // register and finish cleanup.  The next Shutdown call is that turn.
        fake.Reset();
        PjsuaRuntimeAdapter candidate(fake.Api());
        fake.SetThreadRegistered(false);
        fake.FailThreadRegister(PJ_EUNKNOWN);
        assert(candidate.Initialize(registry, fixture.config.security,
                                    fixture.config.conference_format) != Error::ok);
        assert(fake.AccountAddCount() == 0);
        assert(!fake.TeardownSequenceEquals("destroy,reset"));
        assert(PjsuaCallbackRouter::ActiveForTest() != nullptr);
        const auto *retained_router = PjsuaCallbackRouter::ActiveForTest();
        const auto registration_attempts = fake.ThreadRegisterCount();
        assert(candidate.Shutdown() != Error::ok);
        assert(fake.ThreadRegisterCount() == registration_attempts + 1);
        assert(fake.TeardownSequenceEquals(""));
        assert(PjsuaCallbackRouter::ActiveForTest() == retained_router);
        fake.FailThreadRegister(PJ_SUCCESS);
        assert(candidate.Shutdown() == Error::ok);
        assert(fake.TeardownSequenceEquals("destroy,reset"));
        assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);
    };
    if (adapter_limit >= 4) deferred_startup_cleanup();

    auto deferred_native_cleanup = [&]() __attribute__((noinline)) {
        for (bool fail_destroy : {true, false}) {
            fake.Reset();
            // Account creation failed after two callback contexts were bound.
            fake.FailAccountAdd(3);
            if (fail_destroy) fake.Fail(FakePjsuaApi::Stage::destroy);
            else fake.FailArenaReset(PJ_EBUSY);
            PjsuaRuntimeAdapter candidate(fake.Api());
            assert(candidate.Initialize(registry, fixture.config.security,
                                        fixture.config.conference_format) != Error::ok);
            const auto *retained_router = PjsuaCallbackRouter::ActiveForTest();
            assert(retained_router != nullptr);
            assert(candidate.Shutdown() != Error::ok);
            assert(PjsuaCallbackRouter::ActiveForTest() == retained_router);
            if (fail_destroy)
                assert(fake.TeardownSequenceEquals("clear,del,clear,del,close,destroy,destroy"));
            else
                assert(fake.TeardownSequenceEquals("clear,del,clear,del,close,destroy,reset,reset"));
            fake.ClearFailure();
            fake.FailArenaReset(PJ_SUCCESS);
            assert(candidate.Shutdown() == Error::ok);
            assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);
            assert(candidate.Shutdown() == Error::ok);
        }
    };
    if (adapter_limit >= 4) deferred_native_cleanup();

    auto callback_retry_uses_current_actor_time = [&]() __attribute__((noinline)) {
        // The native callback is delivered from handle_events(). Its retry
        // deadline must use this actor turn's time, not the prior turn's.
        fake.Reset();
        PjsuaRuntimeAdapter timed(fake.Api());
        assert(timed.Initialize(registry, fixture.config.security,
                                fixture.config.conference_format) == Error::ok);
        fake.TriggerRegistrationFailureOnNextPump(4);
        assert(timed.Pump(5000, 0) == Error::ok);
        assert(fake.RegistrationCount(4) == 1);
        assert(timed.Pump(6099, 0) == Error::ok);
        assert(fake.RegistrationCount(4) == 1);
        assert(timed.Pump(6100, 0) == Error::ok);
        assert(fake.RegistrationCount(4) == 2);
        assert(timed.Shutdown() == Error::busy);
        assert(timed.Pump(6101, 0) == Error::ok);
        assert(timed.Shutdown() == Error::ok);
    };
    if (adapter_limit >= 4) callback_retry_uses_current_actor_time();

    auto registration_failure = [&]() __attribute__((noinline)) {
    // One initial REGISTER failure is an isolated copied failure/retry pair;
    // it does not undo startup or suppress later enabled accounts.
    fake.Reset(); fake.FailRegistrationFor(2, PJ_EUNKNOWN);
    PjsuaRuntimeAdapter registration_adapter(fake.Api());
    assert(registration_adapter.Initialize(registry, fixture.config.security,
                                           fixture.config.conference_format) == Error::ok);
    RuntimeNotification failure{}; RuntimeNotification retry{};
    assert(registration_adapter.TryGetNotification(&failure));
    assert(registration_adapter.TryGetNotification(&retry));
    assert(failure.agent.slot == 0 && failure.registration == RegistrationState::transport_failed);
    assert(retry.agent.slot == 0 && retry.registration == RegistrationState::retry_wait);
    assert(fake.RegistrationCount(2) == 1);
    assert(fake.RegistrationCount(0) == 1);
    assert(fake.RegistrationCount(4) == 1);
    assert(fake.RegistrationCount(1) == 1);
    assert(fake.RegistrationCount(3) == 0);
    assert(registration_adapter.Shutdown() == Error::busy);
    assert(registration_adapter.Pump(2, 0) == Error::ok);
    assert(registration_adapter.Shutdown() == Error::ok);
    };
    if (adapter_limit >= 5) registration_failure();

    auto notification_pressure = [&]() __attribute__((noinline)) {
    // The bounded adapter ring must not pop manager records while full.  Once
    // space is available, the guaranteed failure/retry pair remains ordered.
    fake.Reset(); PjsuaRuntimeAdapter pressure(fake.Api());
    assert(pressure.Initialize(registry, fixture.config.security,
                               fixture.config.conference_format) == Error::ok);
    pressure.FillNotificationRingForTest();
    fake.DeliverRegistrationFailure(2, PJ_EUNKNOWN);
    assert(pressure.Pump(500, 0) == Error::ok);
    RuntimeNotification staged{};
    for (std::size_t i = 0; i < RuntimeAdapter::notification_capacity; ++i)
        assert(pressure.TryGetNotification(&staged));
    assert(pressure.Pump(501, 0) == Error::ok);
    RuntimeNotification failure{}; RuntimeNotification retry{};
    assert(pressure.TryGetNotification(&failure));
    assert(pressure.TryGetNotification(&retry));
    assert(failure.registration == RegistrationState::transport_failed);
    assert(retry.registration == RegistrationState::retry_wait);
    assert(!pressure.TryGetNotification(&staged));
    assert(pressure.Shutdown() == Error::busy);
    assert(pressure.Pump(502, 0) == Error::ok);
    assert(pressure.Shutdown() == Error::ok);
    };
    if (adapter_limit >= 6) notification_pressure();

    auto shutdown_waits_for_busy_registration = [&]() __attribute__((noinline)) {
        fake.Reset();
        PjsuaRuntimeAdapter pending(fake.Api());
        assert(pending.Initialize(registry, fixture.config.security,
                                  fixture.config.conference_format) == Error::ok);
        fake.HoldRegistrationTransaction(2);
        fake.SetUnregistrationCallbacksDeferred(true);
        assert(pending.Shutdown() == Error::busy);
        fake.DeliverUnregistrationCallbacks();
        const Error blocked = pending.Shutdown();
        printk("PjsuaRuntimeAdapterTest outstanding REGISTER retains shutdown barrier: %s\n",
               blocked == Error::busy ? "yes" : "NO");
        assert(blocked == Error::busy);
        assert(fake.TeardownSequenceEquals(""));
        assert(PjsuaCallbackRouter::ActiveForTest() != nullptr);
        // No unregister was accepted for this account yet. A stale completion
        // cannot discharge the obligation held behind its live REGISTER.
        fake.DeliverRegistrationState(2, PJ_SUCCESS, 200, false, true, 0);
        assert(pending.Pump(100, 0) == Error::ok);
        assert(pending.Shutdown() == Error::busy);
        assert(fake.TeardownSequenceEquals(""));

        // A late successful REGISTER must leave an unregister obligation.
        // The next actor pump can send it, but only its completion permits
        // account deletion, transport close, native destroy, and arena reset.
        fake.DeliverRegistrationState(2, PJ_SUCCESS, 200, true, false, 20);
        assert(pending.Pump(101, 0) == Error::ok);
        assert(fake.PendingUnregistrationCount() == 1);
        const auto requests_after_retry = fake.RegistrationCount(2);
        assert(pending.Shutdown() == Error::busy);
        assert(pending.Pump(102, 0) == Error::ok);
        assert(fake.RegistrationCount(2) == requests_after_retry);
        assert(fake.TeardownSequenceEquals(""));
        fake.DeliverUnregistrationCallbacks();
        assert(pending.Pump(103, 0) == Error::ok);
        assert(pending.Shutdown() == Error::ok);
        assert(fake.TeardownSequenceEquals("clear,del,clear,del,clear,del,clear,del,clear,del,close,destroy,reset"));
        assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);
    };
    if (adapter_limit >= 7) shutdown_waits_for_busy_registration();

    auto shutdown_state_matrix = [&]() __attribute__((noinline)) {
        struct Scenario {
            RegistrationState state;
            pjsua_acc_id selected_native_id;
            std::size_t selected_unregister_delta;
            std::size_t total_unregister_delta;
        };
        const Scenario scenarios[] = {
            {RegistrationState::disabled, 3, 0, 4},
            {RegistrationState::registering, 2, 1, 4},
            {RegistrationState::registered, 2, 1, 4},
            {RegistrationState::retry_wait, 2, 1, 4},
            {RegistrationState::authentication_failed, 2, 0, 3},
            {RegistrationState::transport_failed, 2, 0, 3},
        };
        for (const Scenario &scenario : scenarios) {
            fake.Reset();
            PjsuaRuntimeAdapter matrix(fake.Api());
            assert(matrix.Initialize(registry, fixture.config.security,
                                     fixture.config.conference_format) == Error::ok);
            // IDs 2 and 3 belong respectively to the first enabled and the
            // configured-disabled account. State is reached only through the
            // installed PJSUA callback router.
            switch (scenario.state) {
            case RegistrationState::disabled: break;
            case RegistrationState::registering:
                fake.DeliverRegistrationStarted(scenario.selected_native_id, true); break;
            case RegistrationState::registered:
                fake.DeliverRegistrationState(scenario.selected_native_id, PJ_SUCCESS, 200, true, false, 20); break;
            case RegistrationState::retry_wait:
                fake.DeliverRegistrationState(scenario.selected_native_id, PJ_EUNKNOWN, 0, true, false, 0); break;
            case RegistrationState::authentication_failed:
                fake.DeliverRegistrationState(scenario.selected_native_id, PJ_SUCCESS, 401, true, false, 0); break;
            case RegistrationState::transport_failed:
                fake.DeliverRegistrationState(scenario.selected_native_id, PJ_SUCCESS, 404, true, false, 0); break;
            default: assert(false); break;
            }
            const std::size_t all_before = fake.RegistrationCount();
            const std::size_t selected_before = fake.RegistrationCount(scenario.selected_native_id);
            fake.SetUnregistrationCallbacksDeferred(true);
            assert(matrix.Shutdown() == Error::busy);
            assert(fake.RegistrationCount() == all_before + scenario.total_unregister_delta);
            assert(fake.RegistrationCount(scenario.selected_native_id) == selected_before + scenario.selected_unregister_delta);
            assert(matrix.Shutdown() == Error::busy);
            assert(fake.PendingUnregistrationCount() == scenario.total_unregister_delta);

            if (scenario.selected_unregister_delta != 0) {
                // Completion accounting is token-specific: out-of-order,
                // duplicate, and stale callbacks cannot consume a different
                // account's teardown token.
                fake.DeliverPendingUnregistration(4);
                assert(matrix.Pump(200, 0) == Error::ok);
                assert(fake.PendingUnregistrationCount() == scenario.total_unregister_delta - 1);
                fake.DeliverRegistrationState(4, PJ_SUCCESS, 200, false, true, 0);
                fake.DeliverRegistrationState(PJSUA_INVALID_ID, PJ_SUCCESS, 200, false, true, 0);
                assert(matrix.Shutdown() == Error::busy);
            }
            fake.DeliverUnregistrationCallbacks();
            assert(matrix.Pump(201, 0) == Error::ok);
            assert(matrix.Shutdown() == Error::ok);
            assert(fake.AccountClearCount() == 5 && fake.AccountDeleteCount() == 5);
            assert(fake.TeardownSequenceEquals("clear,del,clear,del,clear,del,clear,del,clear,del,close,destroy,reset"));
            assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);
        }
    };
    if (adapter_limit >= 7) shutdown_state_matrix();

    auto drains_call_notification_after_native_pump_error = [&]() __attribute__((noinline)) {
        // A native callback can have copied an incoming notification before
        // handle_events reports failure. Returning before the call drain loses
        // that notification, so this is intentionally asserted at the adapter
        // boundary rather than against the fake's callback dispatch.
        fake.Reset();
        PjsuaRuntimeAdapter errored(fake.Api());
        assert(errored.Initialize(registry, fixture.config.security,
                                  fixture.config.conference_format) == Error::ok);
        pjsua_call_setting setting{};
        fake.Api().call_setting_default(&setting);
        char uri_text[] = "sip:incoming-error@example.test";
        pj_str_t uri{uri_text, static_cast<pj_ssize_t>(sizeof(uri_text) - 1)};
        pjsua_call_id native_id = PJSUA_INVALID_ID;
        assert(fake.Api().call_make_call(0, &uri, &setting, nullptr, nullptr,
                                         &native_id) == PJ_SUCCESS);
        fake.TriggerIncomingCallOnNextPump(native_id);
        fake.ReturnFromNextPump(-1);
        assert(errored.Pump(300, 0) == Error::internal_failure);
        RuntimeNotification notification{};
        assert(errored.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::incoming_call &&
               notification.token != 0);
        assert(!errored.TryGetNotification(&notification));
        // A pre-pump actor-registration failure remains safe: it cannot enter
        // the native event pump or manufacture a duplicate notification.
        fake.SetThreadRegistered(false);
        fake.FailThreadRegister(PJ_EUNKNOWN);
        assert(errored.Pump(301, 0) == Error::internal_failure);
        assert(!errored.TryGetNotification(&notification));
        fake.SetThreadRegistered(true);
        assert(errored.Shutdown() == Error::busy);
        assert(errored.Shutdown() == Error::ok);
    };
    drains_call_notification_after_native_pump_error();
    fake.Deactivate();
    printk("PjsuaRuntimeAdapterTest PASSED\n");
}
} // namespace voip::test
