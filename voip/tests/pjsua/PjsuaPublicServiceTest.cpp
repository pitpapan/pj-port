#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaApi.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"
#include <voip/VoipService.hpp>

#include <cassert>
#include <new>
#include <zephyr/kernel.h>
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

bool WaitForIncomingGuard(VoipService &service, const FakePjsuaApi &fake,
                          unsigned *incoming_events,
                          unsigned *call_state_events) noexcept {
    for (unsigned attempt = 0; attempt != 200; ++attempt) {
        Event event{};
        while (service.TryGetEvent(&event) == Error::ok) {
            if (event.type == EventType::incoming_call) ++*incoming_events;
            if (event.type == EventType::call_state) ++*call_state_events;
        }
        const ResourceSnapshot resources = service.GetResourceSnapshot();
        if (fake.AnswerCount() == 1 && fake.HangupCount() == 1 &&
            resources.active_calls == 0 && resources.promoted_calls == 0 &&
            resources.queued_calls == 0)
            return true;
        k_sleep(K_MSEC(1));
    }
    return false;
}

bool WaitForRegistered(VoipService &service, AgentHandle agent) noexcept {
    for (unsigned attempt = 0; attempt != 200; ++attempt) {
        AgentSnapshot snapshot{};
        if (service.GetAgentSnapshot(agent, &snapshot) == Error::ok &&
            snapshot.registration == RegistrationState::registered)
            return true;
        Event event{};
        while (service.TryGetEvent(&event) == Error::ok) {}
        k_sleep(K_MSEC(1));
    }
    return false;
}

bool WaitForUnsupportedDial(VoipService &service, OperationId operation,
                            unsigned *matching_terminals,
                            unsigned *matching_call_terminals,
                            CallHandle *terminal_call,
                            std::uint64_t *terminal_call_sequence,
                            std::uint64_t *terminal_operation_sequence) noexcept {
    for (unsigned attempt = 0; attempt != 200; ++attempt) {
        Event event{};
        while (service.TryGetEvent(&event) == Error::ok) {
            if (event.type == EventType::call_state) {
                if (event.operation == operation &&
                    event.transition == CallTransition::timeout) {
                    ++*matching_call_terminals;
                    *terminal_call = event.call;
                    *terminal_call_sequence = event.sequence;
                }
            }
            if (event.type == EventType::operation_terminal && event.operation == operation) {
                assert(event.status.error == Error::unsupported_configuration);
                ++*matching_terminals;
                *terminal_operation_sequence = event.sequence;
            }
        }
        const ResourceSnapshot resources = service.GetResourceSnapshot();
        if (*matching_terminals != 0 && resources.active_calls == 0 &&
            resources.promoted_calls == 0 && resources.queued_calls == 0)
            return true;
        k_sleep(K_MSEC(1));
    }
    return false;
}
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
    fake.TriggerRegisteredOnNextPump(2);
    assert(WaitForRegistered(*first, agent));

    // The fake exposes only an atomic request.  The installed callback is
    // invoked by its actor-owned handle_events/Pump implementation, never by
    // this public-service test thread.
    Drain(*first);
    fake.TriggerIncomingCallOnNextPump(1);
    unsigned incoming_events = 0;
    unsigned incoming_call_states = 0;
    assert(WaitForIncomingGuard(*first, fake, &incoming_events,
                                &incoming_call_states));
    assert(fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    assert(incoming_events == 0 && incoming_call_states == 0);
    assert(first->GetResourceSnapshot().active_calls == 0);

    OperationId dial_operation = 0;
    assert(first->Dial(agent, {"sip:unsupported@example.test"}, &dial_operation) == Error::ok);
    assert(dial_operation != 0);
    unsigned matching_terminals = 0;
    unsigned matching_call_terminals = 0;
    CallHandle terminal_call{};
    std::uint64_t terminal_call_sequence = 0;
    std::uint64_t terminal_operation_sequence = 0;
    assert(WaitForUnsupportedDial(*first, dial_operation, &matching_terminals,
                                  &matching_call_terminals,
                                  &terminal_call,
                                  &terminal_call_sequence,
                                  &terminal_operation_sequence));
    assert(matching_terminals == 1);
    assert(matching_call_terminals == 1);
    assert(terminal_call.IsValid());
    assert(terminal_call_sequence != 0 && terminal_operation_sequence != 0 &&
           terminal_call_sequence < terminal_operation_sequence);
    CallSnapshot terminal_snapshot{};
    assert(first->GetCallSnapshot(terminal_call, &terminal_snapshot) == Error::invalid_handle);
    for (unsigned attempt = 0; attempt != 10; ++attempt) {
        Event event{};
        while (first->TryGetEvent(&event) == Error::ok)
            if (event.type == EventType::operation_terminal && event.operation == dial_operation)
                ++matching_terminals;
        k_sleep(K_MSEC(1));
    }
    assert(matching_terminals == 1);
    assert(first->GetResourceSnapshot().active_calls == 0);
    assert(first->GetResourceSnapshot().available_logical_calls == 7);
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
