#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaApi.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"
#include <voip/VoipService.hpp>

#include <cassert>
#include <new>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
struct ServiceStorage final {
    alignas(VoipService) std::uint8_t bytes[sizeof(VoipService)]{};
};

class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source sources[5]{}; Sink sinks[5]{}; AgentConfig agents[5]{};
    ServiceConfig config{agents, 5, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    Fixture() noexcept { for (std::size_t i = 0; i < 5; ++i) agents[i] = {{i == 0 ? "sip:zero@example.test" : i == 1 ? "sip:one@example.test" : i == 2 ? "sip:two@example.test" : i == 3 ? "sip:three@example.test" : "sip:four@example.test", "sip:registrar.example.test", "user", "password"}, {&sources[i], &sinks[i]}, i != 4}; }
};
void Drain(VoipService &service) noexcept { Event event{}; while (service.TryGetEvent(&event) == Error::ok) {} }
}

void RunPjsuaPublicServiceTests() {
    // A second selected-production service must fail before it can overwrite
    // the static callback route; after first teardown a new service attaches.
    static Fixture fixture;
    FakePjsuaApi fake;
    fake.Activate();
    SetNativePjsuaApiForComponentTest(&fake.Api());
    static ServiceStorage first_storage;
    static ServiceStorage second_storage;
    VoipService *first = ::new (first_storage.bytes) VoipService();
    VoipService *second = ::new (second_storage.bytes) VoipService();
    const Error first_result = first->Initialize(fixture.config);
    assert(first_result == Error::ok);
    assert(second->Initialize(fixture.config) == Error::invalid_state);
    AgentHandle agent{}; assert(first->GetAgentHandle(0, &agent) == Error::ok);
    AgentSnapshot snapshot{}; assert(first->GetAgentSnapshot(agent, &snapshot) == Error::ok);
    assert(first->Shutdown() == Error::ok); Drain(*first);
    assert(second->Initialize(fixture.config) == Error::ok);
    assert(second->Shutdown() == Error::ok); Drain(*second);
    second->~VoipService();
    first->~VoipService();

    // A selected public service retains its callback-reachable adapter after
    // the bounded public wait.  The release signal is application-thread
    // safe, but the fake only delivers its deferred PJSUA callbacks from the
    // actor's next handle_events()/Pump call.
    fake.Reset();
    VoipService *timed = ::new (first_storage.bytes) VoipService();
    assert(timed->Initialize(fixture.config) == Error::ok);
    fake.SetUnregistrationCallbacksDeferred(true);
    assert(timed->Shutdown() == Error::shutdown_timeout);
    assert(fake.AccountClearCount() == 0 && fake.AccountDeleteCount() == 0);
    assert(PjsuaCallbackRouter::ActiveForTest() != nullptr);
    fake.ReleaseUnregistrationCallbacksOnNextPump();
    timed->~VoipService();
    assert(fake.AccountClearCount() == 5 && fake.AccountDeleteCount() == 5);
    assert(fake.UnregistrationCallbacksDeliveredFromPump() == 4);
    assert(fake.PumpCount() != 0);
    assert(fake.TeardownSequenceEquals("clear,del,clear,del,clear,del,clear,del,clear,del,close,destroy,reset"));
    assert(PjsuaCallbackRouter::ActiveForTest() == nullptr);

    fake.Reset();
    VoipService *resumed = ::new (second_storage.bytes) VoipService();
    assert(resumed->Initialize(fixture.config) == Error::ok);
    assert(resumed->Shutdown() == Error::ok);
    Drain(*resumed);
    resumed->~VoipService();
    SetNativePjsuaApiForComponentTest(nullptr);
    printk("PjsuaPublicServiceTest PASSED\n");
}
} // namespace voip::test
