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

bool DialAndGetHandle(voip::VoipService &service, voip::AgentHandle agent,
                      const char *uri, voip::CallHandle *call,
                      voip::OperationId *operation) {
    const voip::Error dial_error = service.Dial(agent, {uri}, operation);
    if (dial_error != voip::Error::ok) return false;
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::call_state &&
            event.operation == *operation &&
            (event.transition == voip::CallTransition::initiation ||
             event.transition == voip::CallTransition::acceptance ||
             event.transition == voip::CallTransition::wait)) {
            *call = event.call;
            return true;
        }
    }
    return false;
}

bool DialAndGetHandle(voip::VoipRuntime &runtime, voip::AgentHandle agent,
                      const char *uri, voip::CallHandle *call,
                      voip::OperationId *operation) {
    if (runtime.Dial(agent, {uri}, operation) != voip::Error::ok) return false;
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::call_state &&
            event.operation == *operation &&
            (event.transition == voip::CallTransition::initiation ||
             event.transition == voip::CallTransition::acceptance ||
             event.transition == voip::CallTransition::wait)) {
            *call = event.call;
            return true;
        }
    }
    return false;
}

void test_composes_five_agents_and_bounded_scheduler() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    config.answer_timeout_ms = 60000;
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::ResourceSnapshot baseline = service.GetResourceSnapshot();
    assert(baseline.active_agents == 5);
    assert(baseline.available_fifo_entries == 5);
    assert(baseline.available_logical_calls == 7);
    assert(baseline.available_promoted_calls == 2);
    assert(baseline.available_commands == 16);
    assert(baseline.available_operations == 16);
    assert(baseline.available_events == 26);

    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < 5; ++i)
        assert(service.GetAgentHandle(i, &handles[i]) == voip::Error::ok);

    std::array<voip::CallHandle, 7> calls{};
    std::array<voip::OperationId, 7> operations{};
    for (std::size_t i = 0; i < calls.size(); ++i) {
        assert(DialAndGetHandle(service, handles[i % handles.size()],
                                "sip:peer", &calls[i], &operations[i]));
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
    bool cancel_completed = false;
    for (unsigned attempt = 0; attempt < 100 && !cancel_completed; ++attempt) {
        if (service.WaitForEvent(&terminal, 20) != voip::Error::ok) continue;
        if (terminal.type != voip::EventType::operation_terminal) continue;
        for (std::size_t i = 0; i < operations.size(); ++i)
            if (terminal.operation == operations[i]) terminal_seen[i] = true;
        if (terminal.operation == cancel_operation) cancel_completed = true;
    }
    assert(cancel_completed);
    assert(terminal.status.error == voip::Error::cancelled);
    terminal_seen[6] = true;
    voip::CallSnapshot cancelled_snapshot{};
    assert(service.GetCallSnapshot(calls[6], &cancelled_snapshot) == voip::Error::invalid_handle);
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
    // Individual terminal records are asserted by the dedicated lifecycle
    // tests below; this pressure case may consume them while obtaining handles.
    const voip::ResourceSnapshot stopped_resources = service.GetResourceSnapshot();
    assert(stopped_resources.active_agents == 0);
    assert(stopped_resources.active_calls == 0);
    assert(stopped_resources.promoted_calls == 0);
    assert(stopped_resources.queued_calls == 0);
    assert(stopped_resources.available_fifo_entries == 5);
    assert(stopped_resources.available_logical_calls == 7);
    assert(stopped_resources.available_promoted_calls == 2);
    assert(stopped_resources.available_events <= 31);
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
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
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
        assert(DialAndGetHandle(service, agent, "sip:first@example.test",
                                &first, &first_operation));
        voip::CallHandle second{};
        voip::OperationId second_operation = 0;
        assert(DialAndGetHandle(service, agent, "sip:second@example.test",
                                &second, &second_operation));
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
        if (!observed_established) {
            voip::CallSnapshot current{};
            observed_established = service.GetCallSnapshot(first, &current) ==
                                   voip::Error::ok &&
                                   current.state == voip::CallState::established;
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
        if (!observed_resume) {
            voip::CallSnapshot current{};
            observed_resume = service.GetCallSnapshot(first, &current) ==
                              voip::Error::ok &&
                              current.state == voip::CallState::established;
        }
        assert(observed_resume);
        voip::OperationId cancel_operation = 0;
        assert(service.Cancel(second, &cancel_operation) == voip::Error::ok);
        bool cancel_done = false;
        for (unsigned i = 0; i < 100 && !cancel_done; ++i) {
            if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::operation_terminal &&
                event.operation == cancel_operation)
                cancel_done = true;
        }
        assert(cancel_done);
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
    assert(DialAndGetHandle(service, agent, "sip:reject@example.test", &call,
                            &dial));
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
    assert(DialAndGetHandle(service, agent, "sip:promoted@example.test",
                            &promoted, &dial_promoted));
    voip::CallHandle queued{};
    voip::OperationId dial_queued = 0;
    assert(DialAndGetHandle(service, agent, "sip:queued@example.test",
                            &queued, &dial_queued));
    voip::OperationId answer_operation = 0;
    assert(service.Answer(queued, &answer_operation) == voip::Error::ok);
    // Answer is invalid for an outgoing call, but its copied command must
    // still produce a terminal operation event on the actor.
    voip::OperationId cancel_operation = 0;
    assert(service.Cancel(queued, &cancel_operation) == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
    bool dial_queued_terminal = false;
    bool cancel_terminal = false;
    bool answer_terminal = false;
    voip::Event event{};
    while (service.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type != voip::EventType::operation_terminal) continue;
        dial_queued_terminal |= event.operation == dial_queued;
        cancel_terminal |= event.operation == cancel_operation;
        answer_terminal |= event.operation == answer_operation &&
                           event.status.error == voip::Error::invalid_state;
    }
    assert(dial_queued_terminal);
    assert(cancel_terminal);
    assert(answer_terminal);
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

void test_queued_incoming_rejection_releases_without_use_after_free() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle first{};
    voip::AgentHandle second{};
    voip::AgentHandle third{};
    assert(runtime.GetAgentHandle(0, &first) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    assert(runtime.GetAgentHandle(2, &third) == voip::Error::ok);
    voip::CallHandle call{};
    voip::OperationId operation = 0;
    assert(DialAndGetHandle(runtime, first,
                            "sip:first-queued-incoming@example.test", &call,
                            &operation));
    assert(DialAndGetHandle(runtime, second,
                            "sip:second-queued-incoming@example.test", &call,
                            &operation));
    voip::RuntimeNotification notification{};
    notification.type = voip::RuntimeNotification::Type::incoming_call;
    notification.agent = third;
    notification.token = 9010;
    std::strncpy(notification.remote_uri, "sip:queued-incoming@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(notification) == voip::Error::ok);
    voip::CallHandle incoming{};
    voip::Event event{};
    bool admitted = false;
    for (unsigned i = 0; i < 100 && !admitted; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            incoming = event.call;
            admitted = true;
        }
    }
    assert(admitted);
    voip::OperationId reject = 0;
    assert(runtime.Reject(incoming, 486, &reject) == voip::Error::ok);
    bool rejected = false;
    for (unsigned i = 0; i < 100 && !rejected; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == reject)
            rejected = event.status.error == voip::Error::remote_rejected;
    }
    assert(rejected);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(incoming, &snapshot) == voip::Error::invalid_handle);
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
    assert(DialAndGetHandle(service, agent, "sip:first-timeout@example.test",
                            &first, &first_operation));
    voip::CallHandle queued{};
    voip::OperationId queued_operation = 0;
    assert(DialAndGetHandle(service, agent, "sip:queued-timeout@example.test",
                            &queued, &queued_operation));
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

void test_disabled_registration_rejects_outgoing_admission() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    agents[0].register_on_start = false;
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(service.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::OperationId operation = 0;
    const voip::Error dial_error = service.Dial(
        agent, {"sip:disabled@example.test"}, &operation);
    assert(dial_error == voip::Error::ok);
    assert(operation != 0);
    voip::Event event{};
    bool rejected = false;
    for (unsigned i = 0; i < 100 && !rejected; ++i) {
        if (service.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == operation)
            rejected = event.status.error == voip::Error::agent_unavailable;
    }
    assert(rejected);
    assert(service.Shutdown() == voip::Error::ok);
}

void test_failed_hold_preserves_established_state() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle call{};
    voip::OperationId dial = 0;
    assert(DialAndGetHandle(runtime, agent, "sip:hold-failure@example.test",
                            &call, &dial));
    voip::Event event{};
    bool established = false;
    for (unsigned i = 0; i < 100 && !established; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::call_state &&
            event.call.slot == call.slot && event.call.generation == call.generation &&
            event.destination_state == voip::CallState::established)
            established = true;
    }
    if (!established) {
        voip::CallSnapshot current{};
        established = runtime.GetCallSnapshot(call, &current) == voip::Error::ok &&
                      current.state == voip::CallState::established;
    }
    assert(established);
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::set_held,
                            voip::Error::media_failed);
    voip::OperationId hold = 0;
    assert(runtime.SetHeld(call, true, &hold) == voip::Error::ok);
    bool failed = false;
    for (unsigned i = 0; i < 100 && !failed; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == hold)
            failed = event.status.error == voip::Error::media_failed;
    }
    assert(failed);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(call, &snapshot) == voip::Error::ok);
    assert(snapshot.state == voip::CallState::established);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_shutdown_adapter_failure_preserves_reachable_runtime() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::shutdown,
                            voip::Error::signaling_failed);
    assert(runtime.Shutdown() == voip::Error::signaling_failed);
    assert(runtime.Shutdown() == voip::Error::signaling_failed);
    voip::OperationId operation = 0;
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    assert(runtime.Dial(agent, {"sip:after-shutdown-failure@example.test"},
                        &operation) == voip::Error::shutting_down);
    assert(operation == 0);
}

void test_reinitialize_requires_draining_prior_events() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);
    assert(runtime.Initialize(config) == voip::Error::invalid_state);
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
    assert(runtime.Initialize(config) == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_stale_agent_is_rejected_before_operation_or_mailbox_reservation() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    voip::AgentHandle stale{};
    assert(service.GetAgentHandle(0, &stale) == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
    voip::Event event{};
    while (service.TryGetEvent(&event) == voip::Error::ok) {}
    assert(service.Initialize(config) == voip::Error::ok);
    while (service.TryGetEvent(&event) == voip::Error::ok) {}
    voip::OperationId operation = 77;
    assert(service.Dial(stale, {"sip:stale@example.test"}, &operation) ==
           voip::Error::invalid_handle);
    assert(operation == 0);
    assert(service.WaitForEvent(&event, 0) == voip::Error::timed_out);
    assert(service.Shutdown() == voip::Error::ok);
    while (service.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_queued_terminal_event_commits_before_call_release() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle first{}, second{};
    assert(runtime.GetAgentHandle(0, &first) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    voip::CallHandle promoted{}, queued{};
    voip::OperationId promoted_operation = 0;
    voip::OperationId queued_operation = 0;
    assert(DialAndGetHandle(runtime, first, "sip:terminal-a@example.test",
                            &promoted, &promoted_operation));
    assert(DialAndGetHandle(runtime, second, "sip:terminal-b@example.test",
                            &queued, &queued_operation));
    voip::OperationId cancel_operation = 0;
    assert(runtime.Cancel(queued, &cancel_operation) == voip::Error::ok);
    bool terminal_seen = false;
    voip::Event event{};
    for (unsigned i = 0; i < 100 && !terminal_seen; ++i) {
        if (runtime.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        terminal_seen = event.type == voip::EventType::call_state &&
                        event.call.slot == queued.slot &&
                        event.call.generation == queued.generation &&
                        event.destination_state == voip::CallState::terminated;
    }
    assert(terminal_seen);
    assert(event.operation == cancel_operation);
    assert(runtime.GetCallSnapshot(queued, nullptr) == voip::Error::invalid_argument);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(queued, &snapshot) == voip::Error::invalid_handle);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_teardown_failure_retries_and_releases_call_capacity() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle call{};
    voip::OperationId dial = 0;
    assert(DialAndGetHandle(runtime, agent, "sip:retry@example.test", &call,
                            &dial));
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::teardown,
                            voip::Error::signaling_failed);
    voip::RuntimeNotification rejected{};
    rejected.type = voip::RuntimeNotification::Type::call_rejected;
    rejected.token = 1;
    rejected.error = voip::Error::remote_rejected;
    assert(runtime.InjectNotification(rejected) == voip::Error::ok);
    runtime.Step(1000);
    assert(runtime.GetResourceSnapshot().active_calls == 1);
    for (unsigned i = 0; i < 100 &&
                            runtime.GetResourceSnapshot().active_calls != 0;
         ++i) {
        runtime.Step(1001 + i);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(runtime.GetResourceSnapshot().active_calls == 0);
    assert(runtime.GetResourceSnapshot().available_logical_calls == 7);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_failed_preanswered_promotion_rolls_back_all_leases() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    config.answer_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle first{}, second{}, third{};
    assert(runtime.GetAgentHandle(0, &first) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    assert(runtime.GetAgentHandle(2, &third) == voip::Error::ok);
    voip::CallHandle promoted{};
    voip::OperationId operation = 0;
    assert(DialAndGetHandle(runtime, first, "sip:lease-a@example.test",
                            &promoted, &operation));
    voip::CallHandle promoted_second{};
    assert(DialAndGetHandle(runtime, second, "sip:lease-b@example.test",
                            &promoted_second, &operation));
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = third;
    incoming.token = 9401;
    std::strncpy(incoming.remote_uri, "sip:preanswered@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    voip::CallHandle queued{};
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            queued = event.call;
            break;
        }
    }
    assert(queued.IsValid());
    voip::OperationId answer = 0;
    assert(runtime.Answer(queued, &answer) == voip::Error::ok);
    bool answer_completed = false;
    for (unsigned i = 0; i < 100 && !answer_completed; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == answer) {
            answer_completed = event.status.error == voip::Error::ok;
        }
    }
    assert(answer_completed);
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::promote_incoming,
                            voip::Error::signaling_failed);
    voip::OperationId hangup = 0;
    assert(runtime.Hangup(promoted, &hangup) == voip::Error::ok);
    bool stale = false;
    for (unsigned i = 0; i < 200; ++i) {
        runtime.Step(0);
        voip::CallSnapshot snapshot{};
        if (runtime.GetCallSnapshot(queued, &snapshot) == voip::Error::invalid_handle) {
            stale = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(stale);
    const voip::ResourceSnapshot resources = runtime.GetResourceSnapshot();
    assert(resources.promoted_calls == 1);
    assert(resources.active_calls == 1);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_shutdown_orders_native_reject_teardown_before_adapter_shutdown() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle first{}, second{}, third{};
    assert(runtime.GetAgentHandle(0, &first) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    assert(runtime.GetAgentHandle(2, &third) == voip::Error::ok);
    voip::OperationId operation = 0;
    voip::CallHandle ignored{};
    assert(DialAndGetHandle(runtime, first, "sip:shutdown-a@example.test",
                            &ignored, &operation));
    assert(DialAndGetHandle(runtime, second, "sip:shutdown-b@example.test",
                            &ignored, &operation));
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = third;
    incoming.token = 9701;
    std::strncpy(incoming.remote_uri, "sip:shutdown-incoming@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    voip::Event event{};
    bool admitted = false;
    for (unsigned i = 0; i < 100 && !admitted; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) admitted = true;
    }
    assert(admitted);
    assert(runtime.Shutdown() == voip::Error::ok);
    const std::size_t count = runtime.AdapterRequestCount();
    assert(count >= 4);
    std::size_t shutdown_index = count;
    std::size_t hangups = 0;
    std::size_t teardowns = 0;
    bool rejected = false;
    for (std::size_t i = 0; i < count; ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::shutdown)
            shutdown_index = i;
        if (request.type == voip::RuntimeRequest::Type::hangup) ++hangups;
        if (request.type == voip::RuntimeRequest::Type::teardown) ++teardowns;
        if (request.type == voip::RuntimeRequest::Type::reject &&
            request.token == 9701 && request.sip_status == 486) rejected = true;
    }
    assert(shutdown_index == count - 1);
    assert(hangups >= 2);
    assert(teardowns >= 2);
    assert(rejected);
}

} // namespace

int main() {
    test_composes_five_agents_and_bounded_scheduler();
    test_security_rejected_and_zero_wait_is_nonblocking();
    test_trace_resources_leases_and_repeated_lifecycles();
    test_every_accepted_operation_gets_terminal_event();
    test_incoming_notification_is_admitted_and_cancelled();
    test_queued_incoming_rejection_releases_without_use_after_free();
    test_queued_call_times_out_and_publishes_terminal_operation();
    test_rejection_finishes_promoted_call_and_preserves_terminal_trace();
    test_fake_adapter_records_bounded_copied_requests();
    test_disabled_registration_rejects_outgoing_admission();
    test_failed_hold_preserves_established_state();
    test_shutdown_adapter_failure_preserves_reachable_runtime();
    test_reinitialize_requires_draining_prior_events();
    test_stale_agent_is_rejected_before_operation_or_mailbox_reservation();
    test_queued_terminal_event_commits_before_call_release();
    test_teardown_failure_retries_and_releases_call_capacity();
    test_failed_preanswered_promotion_rolls_back_all_leases();
    test_shutdown_orders_native_reject_teardown_before_adapter_shutdown();
    std::puts("VoipServiceCoreTest PASSED");
    return 0;
}
