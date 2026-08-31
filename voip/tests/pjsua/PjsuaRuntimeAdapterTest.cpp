#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaRuntimeAdapter.hpp"

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
    Fixture fixture; AgentRegistry registry; assert(registry.Initialize(fixture.config) == Error::ok);
    FakePjsuaApi fake; PjsuaRuntimeAdapter adapter(fake.Api());
    assert(adapter.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::ok);
    assert(fake.AccountAddCount() == 5 && fake.RegistrationCount() == 4);
    assert(adapter.Pump(100, 0) == Error::ok);
    RuntimeNotification notification{};
    assert(!adapter.TryGetNotification(&notification));
    assert(adapter.PromoteOutgoing({}, "sip:peer@example.test", nullptr) == Error::unsupported_configuration);
    assert(adapter.Answer(1) == Error::unsupported_configuration);
    assert(adapter.BeginCallTeardown(1) == Error::unsupported_configuration);
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

    FakePjsuaApi one; PjsuaRuntimeAdapter first(one.Api());
    PjsuaRuntimeAdapter second(one.Api());
    assert(first.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::ok);
    printk("PjsuaRuntimeAdapterTest second lifecycle attached\n");
    assert(second.Initialize(registry, fixture.config.security, fixture.config.conference_format) == Error::invalid_state);
    assert(first.Shutdown() == Error::busy); (void)first.Pump(1, 0); (void)first.Shutdown(); (void)first.Pump(1001, 0); assert(first.Shutdown() == Error::ok);

    FakePjsuaApi immediate_failure;
    PjsuaRuntimeAdapter failed_unregistration(immediate_failure.Api());
    assert(failed_unregistration.Initialize(registry, fixture.config.security,
                                            fixture.config.conference_format) == Error::ok);
    immediate_failure.FailUnregistration(PJ_EUNKNOWN);
    assert(failed_unregistration.Shutdown() == Error::busy);
    // An immediate native failure has no callback to wait for.  Teardown must
    // continue on the next actor step instead of relying on a timeout.
    assert(failed_unregistration.Shutdown() == Error::ok);

    // Startup is transactional at every composition boundary.  These calls
    // deliberately use test-only fake controls added with this matrix.
    for (FakePjsuaApi::Stage stage : {FakePjsuaApi::Stage::arena,
                                      FakePjsuaApi::Stage::create,
                                      FakePjsuaApi::Stage::init,
                                      FakePjsuaApi::Stage::no_sound,
                                      FakePjsuaApi::Stage::transport,
                                      FakePjsuaApi::Stage::start}) {
        FakePjsuaApi startup_failure; startup_failure.Fail(stage);
        PjsuaRuntimeAdapter candidate(startup_failure.Api());
        assert(candidate.Initialize(registry, fixture.config.security,
                                    fixture.config.conference_format) != Error::ok);
        assert(startup_failure.AccountAddCount() == 0);
    }
    for (std::size_t position = 1; position <= 5; ++position) {
        FakePjsuaApi account_failure; account_failure.FailAccountAdd(position);
        PjsuaRuntimeAdapter candidate(account_failure.Api());
        assert(candidate.Initialize(registry, fixture.config.security,
                                    fixture.config.conference_format) != Error::ok);
        assert(account_failure.AccountAddCount() == position);
        assert(account_failure.AccountDeleteCount() == position - 1);
    }

    // One initial REGISTER failure is an isolated copied failure/retry pair;
    // it does not undo startup or suppress later enabled accounts.
    FakePjsuaApi registration_failure; registration_failure.FailRegistrationFor(2, PJ_EUNKNOWN);
    PjsuaRuntimeAdapter registration_adapter(registration_failure.Api());
    assert(registration_adapter.Initialize(registry, fixture.config.security,
                                           fixture.config.conference_format) == Error::ok);
    RuntimeNotification failure{}; RuntimeNotification retry{};
    assert(registration_adapter.TryGetNotification(&failure));
    assert(registration_adapter.TryGetNotification(&retry));
    assert(failure.agent.slot == 0 && failure.registration == RegistrationState::transport_failed);
    assert(retry.agent.slot == 0 && retry.registration == RegistrationState::retry_wait);
    assert(registration_failure.RegistrationCount(2) == 1);
    assert(registration_failure.RegistrationCount(0) == 1);
    assert(registration_failure.RegistrationCount(4) == 1);
    assert(registration_failure.RegistrationCount(1) == 1);
    assert(registration_failure.RegistrationCount(3) == 0);
    assert(registration_adapter.Shutdown() == Error::busy);
    assert(registration_adapter.Pump(2, 0) == Error::ok);
    assert(registration_adapter.Shutdown() == Error::ok);

    // The bounded adapter ring must not pop manager records while full.  Once
    // space is available, the guaranteed failure/retry pair remains ordered.
    FakePjsuaApi pressure_fake; PjsuaRuntimeAdapter pressure(pressure_fake.Api());
    assert(pressure.Initialize(registry, fixture.config.security,
                               fixture.config.conference_format) == Error::ok);
    pressure.FillNotificationRingForTest();
    pressure_fake.DeliverRegistrationFailure(2, PJ_EUNKNOWN);
    assert(pressure.Pump(500, 0) == Error::ok);
    RuntimeNotification staged{};
    for (std::size_t i = 0; i < RuntimeAdapter::notification_capacity; ++i)
        assert(pressure.TryGetNotification(&staged));
    assert(pressure.Pump(501, 0) == Error::ok);
    assert(pressure.TryGetNotification(&failure));
    assert(pressure.TryGetNotification(&retry));
    assert(failure.registration == RegistrationState::transport_failed);
    assert(retry.registration == RegistrationState::retry_wait);
    assert(!pressure.TryGetNotification(&staged));
    assert(pressure.Shutdown() == Error::busy);
    assert(pressure.Pump(502, 0) == Error::ok);
    assert(pressure.Shutdown() == Error::ok);
    printk("PjsuaRuntimeAdapterTest PASSED\n");
}
} // namespace voip::test
