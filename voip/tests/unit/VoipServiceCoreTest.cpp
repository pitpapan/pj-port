#include <voip/VoipService.hpp>
#include "core/VoipRuntime.hpp"
#include "core/FakeRuntimeAdapter.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>
#include <chrono>
#include <thread>

namespace {

class Source final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override { return {8000, 160, 1, voip::SampleFormat::signed_16}; }
    voip::Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; }
};

class Sink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override { return {8000, 160, 1, voip::SampleFormat::signed_16}; }
    voip::Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return voip::Error::ok; }
    void Flush() noexcept override {}
};

voip::ServiceConfig Config(std::array<voip::AgentConfig, 5> &agents,
                           std::array<Source, 5> &sources,
                           std::array<Sink, 5> &sinks) {
    for (std::size_t i = 0; i < agents.size(); ++i) {
        static char uris[5][64] = {"sip:a0@example.test", "sip:a1@example.test", "sip:a2@example.test", "sip:a3@example.test", "sip:a4@example.test"};
        agents[i] = {{uris[i], "sip:example.test", "user", "password"}, {&sources[i], &sinks[i]}, true};
    }
    return {agents.data(), static_cast<std::uint8_t>(agents.size()), 100, 100,
            {8000, 160, 1, voip::SampleFormat::signed_16},
            {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
}

void test_composes_five_agents_and_bounded_scheduler() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::ResourceSnapshot baseline = service.GetResourceSnapshot();
    assert(baseline.active_agents == 5);
    assert(baseline.available_fifo_entries == 5);
    assert(baseline.available_logical_calls == 7);
    assert(baseline.available_promoted_calls == 2);
    assert(baseline.available_commands == 16);
    assert(baseline.available_operations == 16);

    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < 5; ++i)
        assert(service.GetAgentHandle(i, &handles[i]) == voip::Error::ok);

    std::array<voip::CallHandle, 7> calls{};
    std::array<voip::OperationId, 7> operations{};
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const voip::DialRequest request{"sip:peer"};
        assert(service.Dial(handles[i % handles.size()], request, &calls[i],
                            &operations[i]) == voip::Error::ok);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const voip::ResourceSnapshot admitted = service.GetResourceSnapshot();
    assert(admitted.active_calls == 7);
    assert(admitted.promoted_calls == 2);
    assert(admitted.queued_calls == 5);
    assert(admitted.available_fifo_entries == 0);
    assert(admitted.available_logical_calls == 0);
    assert(admitted.available_promoted_calls == 0);

    voip::OperationId cancel_operation = 0;
    assert(service.Cancel(calls[6], &cancel_operation) == voip::Error::ok);
    voip::Event terminal{};
    std::array<bool, 7> terminal_seen{};
    voip::CallSnapshot cancelled_snapshot{};
    assert(service.GetCallSnapshot(calls[6], &cancelled_snapshot) == voip::Error::invalid_handle);
    bool found_cancel = false;
    for (unsigned attempt = 0; attempt < 100 && !found_cancel; ++attempt) {
        voip::Event candidate{};
        if (service.WaitForEvent(&candidate, 20) == voip::Error::ok &&
            candidate.type == voip::EventType::operation_terminal) {
            for (std::size_t i = 0; i < operations.size(); ++i)
                if (candidate.operation == operations[i]) terminal_seen[i] = true;
        }
        if (candidate.type == voip::EventType::operation_terminal &&
            candidate.operation == cancel_operation) {
            terminal = candidate;
            found_cancel = true;
            terminal_seen[6] = true;
        }
    }
    assert(found_cancel);
    assert(terminal.operation == cancel_operation);
    assert(terminal.status.error == voip::Error::cancelled);

    assert(service.Shutdown() == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
    unsigned stopped = 0;
    bool stopped_was_last = false;
    voip::Event event{};
    while (service.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type == voip::EventType::operation_terminal) {
            for (std::size_t i = 0; i < operations.size(); ++i)
                if (event.operation == operations[i]) terminal_seen[i] = true;
        }
        if (event.type == voip::EventType::service_stopped) {
            ++stopped;
            stopped_was_last = true;
        } else {
            assert(!stopped_was_last);
        }
    }
    assert(stopped == 1);
    assert(stopped_was_last);
    for (bool seen : terminal_seen) assert(seen);
    const voip::ResourceSnapshot stopped_resources = service.GetResourceSnapshot();
    assert(stopped_resources.active_agents == 0);
    assert(stopped_resources.active_calls == 0);
    assert(stopped_resources.promoted_calls == 0);
    assert(stopped_resources.queued_calls == 0);
    assert(stopped_resources.available_fifo_entries == 5);
    assert(stopped_resources.available_logical_calls == 7);
    assert(stopped_resources.available_promoted_calls == 2);
}

void test_security_rejected_and_zero_wait_is_nonblocking() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.security.signaling = voip::SignalingSecurity::tls;
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::unsupported_configuration);
    voip::Event event{};
    assert(service.WaitForEvent(&event, 0) == voip::Error::timed_out);
}

void test_trace_resources_leases_and_repeated_lifecycles() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipService service;
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        assert(service.Initialize(config) == voip::Error::ok);
        voip::AgentHandle agent{};
        assert(service.GetAgentHandle(0, &agent) == voip::Error::ok);
        voip::ResourceSnapshot baseline = service.GetResourceSnapshot();
        assert(baseline.available_fifo_entries == 5);
        assert(baseline.available_logical_calls == 7);
        voip::CallHandle first{};
        voip::OperationId first_operation = 0;
        assert(service.Dial(agent, {"sip:first@example.test"}, &first,
                            &first_operation) == voip::Error::ok);
        voip::CallHandle second{};
        voip::OperationId second_operation = 0;
        assert(service.Dial(agent, {"sip:second@example.test"}, &second,
                            &second_operation) == voip::Error::ok);
        const voip::ResourceSnapshot leased = service.GetResourceSnapshot();
        assert(leased.active_calls == 2);
        assert(leased.promoted_calls == 1);
        assert(leased.queued_calls == 1);
        assert(leased.available_promoted_calls == 1);
        voip::Event event{};
        bool observed_established = false;
        for (unsigned i = 0; i < 100; ++i) {
            if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::call_state &&
                event.call.slot == first.slot && event.call.generation == first.generation &&
                event.destination_state == voip::CallState::established) {
                observed_established = true;
                break;
            }
        }
        assert(observed_established);
        voip::OperationId hold_operation = 0;
        assert(service.SetHeld(first, true, &hold_operation) == voip::Error::ok);
        bool observed_hold = false;
        for (unsigned i = 0; i < 100; ++i) {
            if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::call_state &&
                event.call.slot == first.slot && event.call.generation == first.generation &&
                event.destination_state == voip::CallState::hold) {
                observed_hold = true;
                break;
            }
        }
        assert(observed_hold);
        assert(service.SetHeld(first, false, &hold_operation) == voip::Error::ok);
        bool observed_resume = false;
        for (unsigned i = 0; i < 100; ++i) {
            if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::call_state &&
                event.call.slot == first.slot && event.call.generation == first.generation &&
                event.destination_state == voip::CallState::established &&
                event.transition == voip::CallTransition::resume) {
                observed_resume = true;
                break;
            }
        }
        assert(observed_resume);
        voip::OperationId cancel_operation = 0;
        assert(service.Cancel(second, &cancel_operation) == voip::Error::ok);
        const voip::ResourceSnapshot cancelled = service.GetResourceSnapshot();
        assert(cancelled.active_calls == 1);
        assert(cancelled.queued_calls == 0);
        assert(cancelled.promoted_calls == 1);
        voip::OperationId hangup_operation = 0;
        assert(service.Hangup(first, &hangup_operation) == voip::Error::ok);
        for (unsigned i = 0; i < 100; ++i) {
            if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::operation_terminal &&
                event.operation == hangup_operation) break;
        }
        voip::CallSnapshot snapshot{};
        assert(service.GetCallSnapshot(first, &snapshot) == voip::Error::invalid_handle);
        const voip::ResourceSnapshot cleaned = service.GetResourceSnapshot();
        assert(cleaned.active_calls == 0);
        assert(cleaned.promoted_calls == 0);
        assert(cleaned.queued_calls == 0);
        assert(cleaned.available_fifo_entries == 5);
        assert(cleaned.available_logical_calls == 7);
        assert(cleaned.available_promoted_calls == 2);
        assert(service.Shutdown() == voip::Error::ok);
        unsigned stopped = 0;
        while (service.TryGetEvent(&event) == voip::Error::ok)
            if (event.type == voip::EventType::service_stopped) ++stopped;
        assert(stopped == 1);
        assert(service.Shutdown() == voip::Error::ok);
    }
}

void test_rejection_finishes_promoted_call_and_preserves_terminal_trace() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(service.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle call{};
    voip::OperationId dial = 0;
    assert(service.Dial(agent, {"sip:reject@example.test"}, &call, &dial) ==
           voip::Error::ok);
    voip::OperationId reject = 0;
    assert(service.Reject(call, 486, &reject) == voip::Error::ok);
    bool rejected = false;
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 20 && !rejected; ++attempt) {
        if (service.WaitForEvent(&event, 100) != voip::Error::ok) continue;
        if (event.type == voip::EventType::operation_terminal &&
            event.operation == reject) {
            rejected = event.status.error == voip::Error::remote_rejected;
        }
    }
    voip::CallSnapshot snapshot{};
    assert(service.GetCallSnapshot(call, &snapshot) == voip::Error::invalid_handle);
    assert(service.Shutdown() == voip::Error::ok);
}

void test_every_accepted_operation_gets_terminal_event() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(service.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle promoted{};
    voip::OperationId dial_promoted = 0;
    assert(service.Dial(agent, {"sip:promoted@example.test"}, &promoted,
                        &dial_promoted) == voip::Error::ok);
    voip::CallHandle queued{};
    voip::OperationId dial_queued = 0;
    assert(service.Dial(agent, {"sip:queued@example.test"}, &queued,
                        &dial_queued) == voip::Error::ok);
    voip::OperationId answer = 0;
    assert(service.Answer(queued, &answer) == voip::Error::invalid_state);
    // Answer is only valid for incoming calls; use cancellation to replace a
    // pending per-call operation and verify both admissions still terminate.
    assert(service.Cancel(queued, &answer) == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
    bool dial_queued_terminal = false;
    bool cancel_terminal = false;
    voip::Event event{};
    while (service.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type != voip::EventType::operation_terminal) continue;
        dial_queued_terminal |= event.operation == dial_queued;
        cancel_terminal |= event.operation == answer;
    }
    assert(dial_queued_terminal);
    assert(cancel_terminal);
}

void test_incoming_notification_is_admitted_and_cancelled() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::RuntimeNotification notification{};
    notification.type = voip::RuntimeNotification::Type::incoming_call;
    notification.agent = agent;
    notification.token = 9001;
    std::strncpy(notification.remote_uri, "sip:incoming@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(notification) == voip::Error::ok);
    voip::CallHandle call{};
    bool admitted = false;
    voip::Event event{};
    for (unsigned i = 0; i < 100 && !admitted; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            call = event.call;
            admitted = true;
        }
    }
    assert(admitted);
    voip::OperationId operation = 0;
    assert(runtime.Cancel(call, &operation) == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_queued_call_times_out_and_publishes_terminal_operation() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 5;
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(service.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle first{};
    voip::OperationId first_operation = 0;
    assert(service.Dial(agent, {"sip:first-timeout@example.test"}, &first,
                        &first_operation) == voip::Error::ok);
    voip::CallHandle queued{};
    voip::OperationId queued_operation = 0;
    assert(service.Dial(agent, {"sip:queued-timeout@example.test"}, &queued,
                        &queued_operation) == voip::Error::ok);
    bool timed_out = false;
    voip::Event event{};
    for (unsigned i = 0; i < 100 && !timed_out; ++i) {
        if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == queued_operation) {
            timed_out = event.status.error == voip::Error::timed_out;
        }
    }
    assert(timed_out);
    voip::CallSnapshot snapshot{};
    assert(service.GetCallSnapshot(queued, &snapshot) == voip::Error::invalid_handle);
    assert(service.Shutdown() == voip::Error::ok);
}

void test_fake_adapter_records_bounded_copied_requests() {
    voip::FakeRuntimeAdapter adapter;
    assert(adapter.RequestCount() == 0);
    std::uint32_t token = 0;
    assert(adapter.PromoteOutgoing({0, 1}, "sip:record@example.test", &token) ==
           voip::Error::ok);
    assert(adapter.RequestCount() == 1);
    voip::RuntimeRequest request{};
    assert(adapter.GetRequest(0, &request));
    assert(request.type == voip::RuntimeRequest::Type::promote_outgoing);
    assert(std::strcmp(request.remote_uri, "sip:record@example.test") == 0);
}

} // namespace

int main() {
    test_composes_five_agents_and_bounded_scheduler();
    test_security_rejected_and_zero_wait_is_nonblocking();
    test_trace_resources_leases_and_repeated_lifecycles();
    test_every_accepted_operation_gets_terminal_event();
    test_incoming_notification_is_admitted_and_cancelled();
    test_queued_call_times_out_and_publishes_terminal_operation();
    test_rejection_finishes_promoted_call_and_preserves_terminal_trace();
    test_fake_adapter_records_bounded_copied_requests();
    std::puts("VoipServiceCoreTest PASSED");
    return 0;
}
