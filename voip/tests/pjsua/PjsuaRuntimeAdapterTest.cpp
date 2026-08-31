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
    printk("PjsuaRuntimeAdapterTest PASSED\n");
}
} // namespace voip::test
