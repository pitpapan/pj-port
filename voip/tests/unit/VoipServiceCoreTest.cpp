#include <voip/VoipService.hpp>
#include "core/VoipRuntime.hpp"
#include "core/FakeRuntimeAdapter.hpp"

#include <cassert>
#include <atomic>
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
    assert(terminal.status.error == voip::Error::ok);
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

void test_hangup_finishes_promoted_call_and_preserves_terminal_trace() {
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
    assert(service.Hangup(call, &reject) == voip::Error::ok);
    bool rejected = false;
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 20 && !rejected; ++attempt) {
        if (service.WaitForEvent(&event, 100) != voip::Error::ok) continue;
        if (event.type == voip::EventType::operation_terminal &&
            event.operation == reject) {
            // Hangup completes when the actor's native request is
            // accepted; the later copied terminal callback owns teardown.
            rejected = event.status.error == voip::Error::ok;
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
                           (event.status.error == voip::Error::invalid_state ||
                            event.status.error == voip::Error::cancelled);
    }
    assert(dial_queued_terminal);
    assert(cancel_terminal);
    assert(answer_terminal);
}

void test_incoming_notification_is_admitted_and_rejected() {
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
    assert(runtime.Reject(call, 486, &operation) == voip::Error::ok);
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
            rejected = event.status.error == voip::Error::ok;
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
            event.type == voip::EventType::call_state &&
            event.call.slot == queued.slot &&
            event.call.generation == queued.generation)
            timed_out = event.destination_state == voip::CallState::terminated;
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
    assert(adapter.PromoteOutgoing({0, 1}, {0, 1}, "sip:record@example.test", &token) ==
           voip::Error::ok);
    assert(adapter.RequestCount() == 1);
    voip::RuntimeRequest request{};
    assert(adapter.GetRequest(0, &request));
    assert(request.type == voip::RuntimeRequest::Type::promote_outgoing);
    assert(std::strcmp(request.remote_uri, "sip:record@example.test") == 0);
}

void test_outgoing_dial_admission_copies_uri_and_completes_at_durability() {
    std::array<Source, 5> sources; std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    config.answer_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    runtime.SetActorPaused(true); runtime.SetAdapterCallbacksDeferred(true);
    voip::AgentHandle first_agent{}, second_agent{}, queued_agent{}, cancelled_agent{};
    assert(runtime.GetAgentHandle(0, &first_agent) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second_agent) == voip::Error::ok);
    assert(runtime.GetAgentHandle(2, &queued_agent) == voip::Error::ok);
    assert(runtime.GetAgentHandle(3, &cancelled_agent) == voip::Error::ok);
    voip::OperationId first = 0, second = 0, queued = 0, cancelled = 0;
    assert(runtime.Dial(first_agent, {"sip:first@example.test"}, &first) == voip::Error::ok); runtime.Step(1);
    assert(runtime.Dial(second_agent, {"sip:second@example.test"}, &second) == voip::Error::ok); runtime.Step(2);
    const auto outgoing_promotions = [&runtime] {
        std::size_t count = 0;
        for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
            voip::RuntimeRequest request{};
            assert(runtime.GetAdapterRequest(i, &request));
            if (request.type == voip::RuntimeRequest::Type::promote_outgoing) ++count;
        }
        return count;
    };
    const std::size_t promotions_before_queue = outgoing_promotions();
    char copied_uri[voip::max_uri_length + 1] = "sip:copied@example.test";
    assert(runtime.Dial(queued_agent, {copied_uri}, &queued) == voip::Error::ok);
    std::strcpy(copied_uri, "sip:changed@example.test"); runtime.Step(3);
    assert(outgoing_promotions() == promotions_before_queue);
    assert(runtime.Dial(cancelled_agent, {"sip:cancel-before-promotion@example.test"}, &cancelled) == voip::Error::ok); runtime.Step(4);
    assert(outgoing_promotions() == promotions_before_queue);

    // Leaving Dial pending until an acceptance callback makes immediate and
    // FIFO-admitted operations indistinguishable after durable admission.
    std::array<bool, 4> completed{}; voip::Event event{}; voip::CallHandle queued_call{}, cancelled_call{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type == voip::EventType::operation_terminal && event.status.error == voip::Error::ok) {
            if (event.operation == first) completed[0] = true;
            if (event.operation == second) completed[1] = true;
            if (event.operation == queued) completed[2] = true;
            if (event.operation == cancelled) completed[3] = true;
        }
        if (event.type == voip::EventType::call_state && event.operation == queued) queued_call = event.call;
        if (event.type == voip::EventType::call_state && event.operation == cancelled) cancelled_call = event.call;
    }
    assert(completed[0] && completed[1] && completed[2] && completed[3]);
    assert(queued_call.IsValid() && cancelled_call.IsValid());
    const std::size_t before_cancel = runtime.AdapterRequestCount();
    voip::OperationId cancel = 0;
    assert(runtime.Cancel(cancelled_call, &cancel) == voip::Error::ok); runtime.Step(5);
    assert(runtime.AdapterRequestCount() == before_cancel);
    voip::RuntimeNotification finished{};
    finished.type = voip::RuntimeNotification::Type::call_finished;
    finished.token = 1;
    assert(runtime.InjectNotification(finished) == voip::Error::ok);
    runtime.Step(6);
    bool promoted_original_uri = false;
    for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{}; assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::promote_outgoing && std::strcmp(request.remote_uri, "sip:copied@example.test") == 0) promoted_original_uri = true;
    }
    assert(promoted_original_uri);
    finished.token = 2; assert(runtime.InjectNotification(finished) == voip::Error::ok); runtime.Step(7);
    finished.token = 3; assert(runtime.InjectNotification(finished) == voip::Error::ok); runtime.Step(8);
    runtime.SetActorPaused(false); assert(runtime.Shutdown() == voip::Error::ok);
}

void test_failed_first_effect_does_not_skip_second_in_single_capacity_pass() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    runtime.SetActorPaused(true);
    runtime.SetAdapterCallbacksDeferred(true);

    std::array<voip::AgentHandle, 5> agent{};
    for (std::uint8_t index = 0; index < agent.size(); ++index)
        assert(runtime.GetAgentHandle(index, &agent[index]) == voip::Error::ok);

    voip::OperationId first_live_operation = 0;
    voip::OperationId second_live_operation = 0;
    assert(runtime.Dial(agent[0], {"sip:live-first@example.test"},
                        &first_live_operation) == voip::Error::ok);
    runtime.Step(1);
    assert(runtime.Dial(agent[1], {"sip:live-second@example.test"},
                        &second_live_operation) == voip::Error::ok);
    runtime.Step(2);

    // Hold promotion while both original leases drain. Re-enabling it below
    // makes OnCapacityChanged create one two-effect FIFO batch.
    runtime.SetSchedulerPromotionEnabledForTest(false);
    voip::OperationId first_queued_operation = 0;
    voip::OperationId second_queued_operation = 0;
    voip::OperationId third_queued_operation = 0;
    assert(runtime.Dial(agent[2], {"sip:effect-first@example.test"},
                        &first_queued_operation) == voip::Error::ok);
    runtime.Step(3);
    assert(runtime.Dial(agent[3], {"sip:effect-second@example.test"},
                        &second_queued_operation) == voip::Error::ok);
    runtime.Step(4);
    assert(runtime.Dial(agent[4], {"sip:effect-third@example.test"},
                        &third_queued_operation) == voip::Error::ok);
    runtime.Step(4);

    voip::CallHandle first_queued{};
    voip::CallHandle second_queued{};
    voip::CallHandle third_queued{};
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type != voip::EventType::call_state) continue;
        if (event.operation == first_queued_operation) first_queued = event.call;
        if (event.operation == second_queued_operation) second_queued = event.call;
        if (event.operation == third_queued_operation) third_queued = event.call;
    }
    assert(first_queued.IsValid() && second_queued.IsValid() && third_queued.IsValid());

    voip::RuntimeNotification finished{};
    finished.type = voip::RuntimeNotification::Type::call_finished;
    finished.token = 1;
    assert(runtime.InjectNotification(finished) == voip::Error::ok);
    finished.token = 2;
    assert(runtime.InjectNotification(finished) == voip::Error::ok);
    runtime.Step(5);
    assert(runtime.GetResourceSnapshot().promoted_calls == 0);
    assert(runtime.GetResourceSnapshot().queued_calls == 3);

    const std::size_t before = runtime.AdapterRequestCount();
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::promote_outgoing,
                            voip::Error::signaling_failed);
    runtime.SetSchedulerPromotionEnabledForTest(true);
    assert(runtime.RunCapacityPassForTest() == 2);
    assert(runtime.AdapterRequestCount() == before + 3);

    voip::RuntimeRequest first_request{};
    voip::RuntimeRequest second_request{};
    voip::RuntimeRequest third_request{};
    assert(runtime.GetAdapterRequest(before, &first_request));
    assert(runtime.GetAdapterRequest(before + 1, &second_request));
    assert(runtime.GetAdapterRequest(before + 2, &third_request));
    assert(first_request.type == voip::RuntimeRequest::Type::promote_outgoing);
    assert(second_request.type == voip::RuntimeRequest::Type::promote_outgoing);
    assert(third_request.type == voip::RuntimeRequest::Type::promote_outgoing);
    assert(first_request.call.slot == first_queued.slot);
    assert(first_request.call.generation == first_queued.generation);
    assert(second_request.call.slot == second_queued.slot);
    assert(second_request.call.generation == second_queued.generation);
    assert(third_request.call.slot == third_queued.slot);
    assert(third_request.call.generation == third_queued.generation);
    assert(std::strcmp(first_request.remote_uri, "sip:effect-first@example.test") == 0);
    assert(std::strcmp(second_request.remote_uri, "sip:effect-second@example.test") == 0);
    assert(std::strcmp(third_request.remote_uri, "sip:effect-third@example.test") == 0);

    bool first_terminal_seen = false;
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type == voip::EventType::call_state &&
            event.call.slot == first_queued.slot &&
            event.call.generation == first_queued.generation &&
            event.transition == voip::CallTransition::timeout) {
            assert(event.operation == first_queued_operation);
            first_terminal_seen = true;
        }
    }
    assert(first_terminal_seen);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(first_queued, &snapshot) == voip::Error::invalid_handle);
    runtime.DrainAdapterCallbacks();
    runtime.Step(6);
    assert(runtime.GetCallSnapshot(second_queued, &snapshot) == voip::Error::ok);
    assert(snapshot.state == voip::CallState::established);
    assert(runtime.GetCallSnapshot(third_queued, &snapshot) == voip::Error::ok);
    assert(snapshot.state == voip::CallState::established);
    runtime.SetAdapterCallbacksDeferred(false);
    runtime.SetActorPaused(false);
    assert(runtime.Shutdown() == voip::Error::ok);
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
    assert(dial_error == voip::Error::agent_unavailable);
    assert(operation == 0);
    voip::Event event{};
    bool terminal = false;
    while (service.TryGetEvent(&event) == voip::Error::ok)
        terminal |= event.type == voip::EventType::operation_terminal;
    assert(!terminal);
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
    assert(runtime.Shutdown() == voip::Error::ok);
    assert(runtime.AdapterRequestCount() >= 2);
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

void test_initialization_failure_rolls_back_adapter_and_events() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::initialize,
                             voip::Error::signaling_failed);
    assert(runtime.Initialize(config) == voip::Error::signaling_failed);
    voip::AgentHandle handle{};
    assert(runtime.GetAgentHandle(0, &handle) == voip::Error::invalid_argument);
    voip::Event event{};
    assert(runtime.TryGetEvent(&event) == voip::Error::timed_out);
    assert(runtime.Initialize(config) == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_initialize_waits_for_actor_bootstrap_without_holding_runtime_mutex() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < handles.size(); ++i)
        assert(runtime.GetAgentHandle(i, &handles[i]) == voip::Error::ok);
    voip::Event event{};
    std::size_t snapshots = 0;
    while (runtime.TryGetEvent(&event) == voip::Error::ok)
        if (event.type == voip::EventType::agent_snapshot) ++snapshots;
    assert(snapshots == 5);
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_bootstrap_drains_at_most_one_notification_capacity() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    runtime.SetActorPaused(true);
    runtime.SetAdapterInitializationNotificationBurst(
        voip::RuntimeAdapter::notification_capacity);
    assert(runtime.Initialize(config) == voip::Error::ok);
    assert(runtime.PendingAdapterNotifications() ==
           5);
    runtime.SetActorPaused(false);
    assert(runtime.Shutdown() == voip::Error::ok);
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_registration_notifications_update_only_the_matching_agent() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);

    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < handles.size(); ++i)
        assert(runtime.GetAgentHandle(i, &handles[i]) == voip::Error::ok);
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}

    const std::array<voip::RegistrationState, 5> states{
        voip::RegistrationState::registered,
        voip::RegistrationState::refreshing,
        voip::RegistrationState::authentication_failed,
        voip::RegistrationState::transport_failed,
        voip::RegistrationState::disabled,
    };
    for (std::size_t i = 0; i < handles.size(); ++i) {
        voip::RuntimeNotification notification{};
        notification.type = voip::RuntimeNotification::Type::registration_state;
        notification.agent = handles[i];
        notification.registration = states[i];
        notification.status = {i == 2 ? voip::Error::authentication_failed
                                      : voip::Error::ok,
                               static_cast<std::uint16_t>(i == 2 ? 401 : 200), {}};
        assert(runtime.InjectNotification(notification) == voip::Error::ok);
    }

    std::array<bool, 5> seen{};
    for (std::size_t attempts = 0; attempts < 100 &&
                                   !(seen[0] && seen[1] && seen[2] && seen[3] && seen[4]);
         ++attempts) {
        if (runtime.WaitForEvent(&event, 20) != voip::Error::ok ||
            event.type != voip::EventType::agent_snapshot)
            continue;
        for (std::size_t i = 0; i < handles.size(); ++i) {
            if (event.agent.slot != handles[i].slot ||
                event.agent.generation != handles[i].generation)
                continue;
            assert(event.agent_snapshot.registration == states[i]);
            assert(event.status.error == (i == 2 ? voip::Error::authentication_failed
                                                  : voip::Error::ok));
            assert(event.status.sip_status == (i == 2 ? 401 : 200));
            seen[i] = true;
        }
    }
    for (bool value : seen) assert(value);
    voip::AgentSnapshot snapshot{};
    for (std::size_t i = 0; i < handles.size(); ++i) {
        assert(runtime.GetAgentSnapshot(handles[i], &snapshot) == voip::Error::ok);
        assert(snapshot.registration == states[i]);
    }
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_invalid_promoted_answer_does_not_call_adapter() {
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
    assert(DialAndGetHandle(runtime, agent, "sip:invalid-answer@example.test",
                            &call, &dial));
    const std::size_t before = runtime.AdapterRequestCount();
    voip::OperationId answer = 0;
    assert(runtime.Answer(call, &answer) == voip::Error::ok);
    bool invalid = false;
    voip::Event event{};
    for (unsigned i = 0; i < 100 && !invalid; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == answer)
            invalid = event.status.error == voip::Error::invalid_state;
    }
    assert(invalid);
    for (std::size_t i = before; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        assert(request.type != voip::RuntimeRequest::Type::answer);
    }
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_queued_incoming_reject_uses_native_callback_before_release() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle active{};
    voip::OperationId dial = 0;
    assert(DialAndGetHandle(runtime, agent, "sip:active-hangup@example.test",
                            &active, &dial));
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = agent;
    incoming.token = 9901;
    std::strncpy(incoming.remote_uri, "sip:queued-hangup@example.test",
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
    voip::OperationId hangup = 0;
    assert(runtime.Reject(queued, 486, &hangup) == voip::Error::ok);
    bool completed = false;
    for (unsigned i = 0; i < 100 && !completed; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::operation_terminal &&
            event.operation == hangup)
            completed = event.status.error == voip::Error::ok;
    }
    assert(completed);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(queued, &snapshot) == voip::Error::invalid_handle);
    assert(runtime.Shutdown() == voip::Error::ok);
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
    assert(DialAndGetHandle(runtime, first, "sip:terminal-b@example.test",
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

void test_failed_promotion_commits_terminal_before_release() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    runtime.FailNextAdapter(voip::RuntimeRequest::Type::promote_outgoing,
                            voip::Error::signaling_failed);
    voip::OperationId operation = 0;
    voip::CallHandle call{};
    assert(runtime.Dial(agent, {"sip:promotion-failure@example.test"},
                        &operation) == voip::Error::ok);
    bool terminal = false;
    voip::Event event{};
    for (unsigned i = 0; i < 100 && !terminal; ++i) {
        if (runtime.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::call_state &&
            event.operation == operation &&
            event.transition == voip::CallTransition::timeout) {
            call = event.call;
            terminal = true;
        }
    }
    assert(terminal);
    assert(call.IsValid());
    assert(runtime.GetCallSnapshot(call, nullptr) == voip::Error::invalid_argument);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(call, &snapshot) == voip::Error::invalid_handle);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_shutdown_waits_for_existing_teardown_callback() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    runtime.SetActorPaused(true);
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::CallHandle call{};
    voip::AgentHandle second_agent{};
    assert(runtime.GetAgentHandle(1, &second_agent) == voip::Error::ok);
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = second_agent;
    incoming.token = 9911;
    std::strncpy(incoming.remote_uri, "sip:deferred-teardown@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    runtime.Step(1000);
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            call = event.call;
            break;
        }
    }
    assert(call.IsValid());
    runtime.SetAdapterCallbacksDeferred(true);
    voip::RuntimeNotification rejected{};
    rejected.type = voip::RuntimeNotification::Type::call_rejected;
    rejected.token = incoming.token;
    rejected.error = voip::Error::remote_rejected;
    // Injected callbacks are delivered immediately; callbacks generated by
    // the native rejection/teardown request remain deferred.
    assert(runtime.InjectNotification(rejected) == voip::Error::ok);
    runtime.Step(1000);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(call, &snapshot) == voip::Error::ok);
    runtime.SetActorPaused(false);
    assert(runtime.Shutdown() == voip::Error::shutdown_timeout);
    runtime.DrainAdapterCallbacks();
    assert(runtime.Shutdown() == voip::Error::ok);
    std::size_t hangups = 0;
    std::size_t teardowns = 0;
    std::size_t shutdown_index = 0;
    const std::size_t count = runtime.AdapterRequestCount();
    for (std::size_t i = 0; i < count; ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::hangup) ++hangups;
        if (request.type == voip::RuntimeRequest::Type::teardown) ++teardowns;
        if (request.type == voip::RuntimeRequest::Type::shutdown)
            shutdown_index = i;
    }
    assert(hangups == 0);
    assert(teardowns == 1);
    assert(shutdown_index == count - 1);
}

void test_shutdown_waits_for_user_native_terminal_requests() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    config.answer_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    runtime.SetActorPaused(true);
    assert(runtime.Initialize(config) == voip::Error::ok);
    std::array<voip::AgentHandle, 3> handles{};
    for (std::uint8_t i = 0; i < handles.size(); ++i)
        assert(runtime.GetAgentHandle(i, &handles[i]) == voip::Error::ok);
    runtime.SetAdapterCallbacksDeferred(true);
    auto admit = [&](voip::AgentHandle agent, std::uint32_t token,
                     const char *uri) {
        voip::RuntimeNotification incoming{};
        incoming.type = voip::RuntimeNotification::Type::incoming_call;
        incoming.agent = agent;
        incoming.token = token;
        std::strncpy(incoming.remote_uri, uri, voip::max_uri_length);
        assert(runtime.InjectNotification(incoming) == voip::Error::ok);
        runtime.Step(1000);
        voip::Event event{};
        for (unsigned i = 0; i < 100; ++i) {
            if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::incoming_call)
                return event.call;
        }
        return voip::CallHandle{};
    };
    const voip::CallHandle hung_up =
        admit(handles[0], 1103, "sip:user-hangup@example.test");
    voip::RuntimeNotification accepted{};
    accepted.type = voip::RuntimeNotification::Type::call_accepted;
    accepted.token = 1103;
    assert(runtime.InjectNotification(accepted) == voip::Error::ok);
    voip::OperationId dial{};
    assert(runtime.Dial(handles[1], {"sip:user-cancel@example.test"}, &dial) == voip::Error::ok);
    runtime.Step(1000);
    voip::CallHandle cancelled{};
    voip::Event admission{};
    while (runtime.TryGetEvent(&admission) == voip::Error::ok)
        if (admission.type == voip::EventType::call_state && admission.operation == dial)
            cancelled = admission.call;
    const voip::CallHandle rejected =
        admit(handles[2], 1101, "sip:user-reject@example.test");
    assert(rejected.IsValid() && cancelled.IsValid() && hung_up.IsValid());
    voip::OperationId reject_operation = 0;
    voip::OperationId cancel_operation = 0;
    voip::OperationId hangup_operation = 0;
    assert(runtime.Reject(rejected, 486, &reject_operation) == voip::Error::ok);
    assert(runtime.Cancel(cancelled, &cancel_operation) == voip::Error::ok);
    assert(runtime.Hangup(hung_up, &hangup_operation) == voip::Error::ok);
    runtime.Step(1000);
    assert(runtime.GetCallSnapshot(rejected, nullptr) == voip::Error::invalid_argument);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(rejected, &snapshot) == voip::Error::ok);
    assert(runtime.GetCallSnapshot(cancelled, &snapshot) == voip::Error::ok);
    assert(runtime.GetCallSnapshot(hung_up, &snapshot) == voip::Error::ok);
    runtime.SetActorPaused(false);
    assert(runtime.Shutdown() == voip::Error::shutdown_timeout);
    runtime.SetAdapterCallbacksDeferred(false);
    runtime.DrainAdapterCallbacks();
    assert(runtime.Shutdown() == voip::Error::ok);
    std::size_t rejects = 0;
    std::size_t cancels = 0;
    std::size_t hangups = 0;
    std::size_t shutdown_index = 0;
    const std::size_t count = runtime.AdapterRequestCount();
    for (std::size_t i = 0; i < count; ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::reject &&
            request.token == 1101) ++rejects;
        if (request.type == voip::RuntimeRequest::Type::cancel &&
            request.token == 1) ++cancels;
        if (request.type == voip::RuntimeRequest::Type::hangup &&
            request.token == 1103) ++hangups;
        if (request.type == voip::RuntimeRequest::Type::shutdown)
            shutdown_index = i;
    }
    assert(rejects == 1);
    assert(cancels == 1);
    assert(hangups == 1);
    assert(shutdown_index == count - 1);
}

void test_concurrent_shutdown_callers_are_serialized() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::Error first_result = voip::Error::internal_failure;
    voip::Error second_result = voip::Error::internal_failure;
    std::thread first([&] { first_result = runtime.Shutdown(); });
    std::thread second([&] { second_result = runtime.Shutdown(); });
    first.join();
    second.join();
    assert(first_result == voip::Error::ok);
    assert(second_result == voip::Error::ok);
    std::size_t shutdowns = 0;
    for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::shutdown) ++shutdowns;
    }
    assert(shutdowns == 1);
}

void test_initialize_waits_for_shutdown_coordination() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    config.queue_timeout_ms = 60000;
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(1, &agent) == voip::Error::ok);
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = agent;
    incoming.token = 9931;
    std::strncpy(incoming.remote_uri, "sip:lifecycle-lock@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    voip::CallHandle call{};
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            call = event.call;
            break;
        }
    }
    assert(call.IsValid());
    runtime.SetAdapterCallbacksDeferred(true);
    std::atomic<bool> initialize_done{false};
    voip::Error shutdown_result = voip::Error::internal_failure;
    voip::Error initialize_result = voip::Error::internal_failure;
    std::thread shutdown_thread([&] {
        shutdown_result = runtime.Shutdown();
    });
    bool hangup_started = false;
    for (unsigned i = 0; i < 1000 && !hangup_started; ++i) {
        for (std::size_t request_index = 0;
             request_index < runtime.AdapterRequestCount(); ++request_index) {
            voip::RuntimeRequest request{};
            if (!runtime.GetAdapterRequest(request_index, &request)) continue;
            if (request.type == voip::RuntimeRequest::Type::hangup &&
                request.token == incoming.token) {
                hangup_started = true;
                break;
            }
        }
        if (!hangup_started)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(hangup_started);
    std::thread initialize_thread([&] {
        initialize_result = runtime.Initialize(config);
        initialize_done.store(true);
    });
    for (unsigned i = 0; i < 100 && !initialize_done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!initialize_done.load());
    runtime.SetAdapterCallbacksDeferred(false);
    runtime.DrainAdapterCallbacks();
    shutdown_thread.join();
    initialize_thread.join();
    assert(shutdown_result == voip::Error::ok);
    assert(initialize_result == voip::Error::invalid_state);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
    assert(runtime.Initialize(config) == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_fake_callback_state_resets_between_lifecycles() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    runtime.SetAdapterCallbacksDeferred(true);
    assert(runtime.Shutdown() == voip::Error::ok);
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
    voip::CallHandle call{};
    voip::OperationId operation = 0;
    assert(DialAndGetHandle(runtime, agent, "sip:fresh-adapter-state@example.test",
                            &call, &operation));
    runtime.Step(0);
    voip::CallSnapshot snapshot{};
    assert(runtime.GetCallSnapshot(call, &snapshot) == voip::Error::ok);
    assert(snapshot.state == voip::CallState::established);
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_shutdown_timeout_requires_retry_before_reinitialize() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle agent{};
    assert(runtime.GetAgentHandle(1, &agent) == voip::Error::ok);
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = agent;
    incoming.token = 9951;
    std::strncpy(incoming.remote_uri, "sip:retry-lifecycle@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    voip::CallHandle call{};
    voip::Event event{};
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call) {
            call = event.call;
            break;
        }
    }
    assert(call.IsValid());
    runtime.SetAdapterCallbacksDeferred(true);
    assert(runtime.Shutdown() == voip::Error::shutdown_timeout);
    runtime.SetAdapterCallbacksDeferred(false);
    runtime.DrainAdapterCallbacks();
    bool stopped_event = false;
    for (unsigned i = 0; i < 100; ++i) {
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::service_stopped) {
            stopped_event = true;
            break;
        }
    }
    assert(stopped_event);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
    assert(runtime.Initialize(config) == voip::Error::invalid_state);
    assert(runtime.Shutdown() == voip::Error::ok);
    std::size_t shutdowns = 0;
    for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::shutdown) ++shutdowns;
    }
    assert(shutdowns == 1);
    assert(runtime.Initialize(config) == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {}
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
    assert(hangups == 2);
    assert(teardowns == 0);
    assert(rejected);
}

void test_incoming_admission_orders_native_180_and_hides_failure() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = Config(agents, sources, sinks);
    voip::VoipRuntime runtime;
    assert(runtime.Initialize(config) == voip::Error::ok);
    voip::AgentHandle first{}, second{}, third{};
    assert(runtime.GetAgentHandle(0, &first) == voip::Error::ok);
    assert(runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    assert(runtime.GetAgentHandle(2, &third) == voip::Error::ok);
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.agent = first;
    incoming.token = 12001;
    std::strncpy(incoming.remote_uri, "sip:ordered-incoming@example.test",
                 voip::max_uri_length);
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    voip::Event event{};
    voip::CallHandle immediate{};
    for (unsigned i = 0; i < 100 && !immediate.IsValid(); ++i)
        if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
            event.type == voip::EventType::incoming_call)
            immediate = event.call;
    assert(immediate.IsValid());
    std::size_t admit_index = runtime.AdapterRequestCount();
    std::size_t promote_index = runtime.AdapterRequestCount();
    for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        if (request.type == voip::RuntimeRequest::Type::admit_incoming &&
            request.call.slot == immediate.slot)
            admit_index = i;
        if (request.type == voip::RuntimeRequest::Type::promote_incoming &&
            request.call.slot == immediate.slot)
            promote_index = i;
    }
    assert(admit_index < promote_index);

    runtime.FailNextAdapter(voip::RuntimeRequest::Type::admit_incoming,
                            voip::Error::signaling_failed);
    incoming.agent = second;
    incoming.token = 12002;
    assert(runtime.InjectNotification(incoming) == voip::Error::ok);
    for (unsigned i = 0; i < 10; ++i)
        runtime.Step(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    assert(runtime.GetResourceSnapshot().active_calls == 1);
    bool rejected = false;
    for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
        voip::RuntimeRequest request{};
        assert(runtime.GetAdapterRequest(i, &request));
        rejected |= request.type == voip::RuntimeRequest::Type::abort_incoming &&
                    request.token == 12002 && request.sip_status == 486;
    }
    assert(rejected);
    assert(runtime.Shutdown() == voip::Error::ok);
}

void test_prepublication_failures_abort_incoming_and_restore_reservations() {
    unsigned routing_failures = 0;
    for (bool fail_commit : {false, true}) {
        std::array<Source, 5> sources;
        std::array<Sink, 5> sinks;
        std::array<voip::AgentConfig, 5> agents{};
        const voip::ServiceConfig config = Config(agents, sources, sinks);
        voip::VoipRuntime runtime;
        assert(runtime.Initialize(config) == voip::Error::ok);
        voip::AgentHandle agent{};
        assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
        const std::uint16_t event_baseline =
            runtime.GetResourceSnapshot().available_events;
        if (fail_commit)
            runtime.FailNextIncomingEventCommitForTest();
        else
            runtime.FailNextAdapter(voip::RuntimeRequest::Type::admit_incoming,
                                    voip::Error::signaling_failed);
        voip::RuntimeNotification incoming{};
        incoming.type = voip::RuntimeNotification::Type::incoming_call;
        incoming.agent = agent;
        incoming.token = 12101;
        std::strncpy(incoming.remote_uri, "sip:commit-failure@example.test",
                     voip::max_uri_length);
        assert(runtime.InjectNotification(incoming) == voip::Error::ok);
        runtime.Step(0);
        const voip::ResourceSnapshot after_failure = runtime.GetResourceSnapshot();
        assert(after_failure.active_calls == 0 &&
               after_failure.available_logical_calls == 7 &&
               after_failure.available_events == event_baseline);
        unsigned aborts = 0;
        unsigned rejects = 0;
        for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
            voip::RuntimeRequest request{};
            assert(runtime.GetAdapterRequest(i, &request));
            if (request.token != 12101) continue;
            if (request.type == voip::RuntimeRequest::Type::abort_incoming &&
                request.sip_status == 486) ++aborts;
            if (request.type == voip::RuntimeRequest::Type::reject) ++rejects;
            assert(request.type != voip::RuntimeRequest::Type::promote_incoming);
        }
        if (aborts != 1 || rejects != 0) {
            std::fprintf(stderr, "%s failure: expected one private abort and no "
                         "generic reject; got %u aborts, %u rejects\n",
                         fail_commit ? "incoming commit" : "native admission/180",
                         aborts, rejects);
            ++routing_failures;
        }
        voip::Event event{};
        while (runtime.TryGetEvent(&event) == voip::Error::ok) {
            assert(event.type != voip::EventType::incoming_call);
            assert(event.type != voip::EventType::call_state);
        }
        // A second admission proves the first terminal reservation was cancelled;
        // its exact resource baseline prevents masking a leaked reservation.
        incoming.token = 12102;
        assert(runtime.InjectNotification(incoming) == voip::Error::ok);
        bool admitted = false;
        for (unsigned i = 0; i < 100 && !admitted; ++i)
            if (runtime.WaitForEvent(&event, 20) == voip::Error::ok &&
                event.type == voip::EventType::incoming_call)
                admitted = true;
        assert(admitted);
        assert(runtime.GetResourceSnapshot().available_logical_calls == 6);
        assert(runtime.Shutdown() == voip::Error::ok);
    }
    assert(routing_failures == 0);
}

// All actor iterations in these control/timer tests use one synthetic clock.
// Native request acceptance and copied callback delivery can be observed apart.
struct ControlFixture {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    voip::VoipRuntime runtime;
    voip::AgentHandle agent{};
    std::uint64_t now = 1000;
    std::array<voip::Event, 256> events{};
    std::size_t count = 0;

    ControlFixture(std::uint32_t queue = 1000, std::uint32_t answer = 1000) {
        auto config = Config(agents, sources, sinks);
        config.queue_timeout_ms = queue;
        config.answer_timeout_ms = answer;
        runtime.SetActorPaused(true);
        assert(runtime.Initialize(config) == voip::Error::ok);
        runtime.SetAdapterCallbacksDeferred(true);
        assert(runtime.GetAgentHandle(0, &agent) == voip::Error::ok);
        Step(now);
    }
    ~ControlFixture() {
        runtime.SetAdapterCallbacksDeferred(false);
        runtime.DrainAdapterCallbacks();
        runtime.SetActorPaused(false);
        assert(runtime.Shutdown() == voip::Error::ok);
    }
    void Collect() {
        voip::Event event{};
        while (runtime.TryGetEvent(&event) == voip::Error::ok) {
            assert(count < events.size());
            events[count++] = event;
        }
    }
    void Step(std::uint64_t time) { now = time; runtime.Step(now); Collect(); }
    void Notify(voip::RuntimeNotification::Type type, std::uint32_t token) {
        voip::RuntimeNotification notification{};
        notification.type = type; notification.token = token;
        notification.agent = agent;
        std::strcpy(notification.remote_uri, "sip:control@example.test");
        assert(runtime.InjectNotification(notification) == voip::Error::ok);
        Step(now);
    }
    voip::CallHandle Incoming(std::uint32_t token) {
        const auto before = count;
        Notify(voip::RuntimeNotification::Type::incoming_call, token);
        for (auto i = before; i < count; ++i)
            if (events[i].type == voip::EventType::incoming_call) return events[i].call;
        assert(false); return {};
    }
    voip::CallHandle Outgoing() {
        voip::OperationId operation{};
        assert(runtime.Dial(agent, {"sip:control@example.test"}, &operation) == voip::Error::ok);
        const auto before = count; Step(now);
        for (auto i = before; i < count; ++i)
            if (events[i].type == voip::EventType::call_state && events[i].operation == operation)
                return events[i].call;
        assert(false); return {};
    }
    unsigned Completions(voip::OperationId operation, voip::Error expected) const {
        unsigned found = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (events[i].type == voip::EventType::operation_terminal && events[i].operation == operation) {
                assert(events[i].status.error == expected); ++found;
            }
        return found;
    }
    unsigned Requests(voip::RuntimeRequest::Type type, std::uint32_t token = 0,
                      unsigned status = 0) const {
        unsigned found = 0;
        for (std::size_t i = 0; i < runtime.AdapterRequestCount(); ++i) {
            voip::RuntimeRequest request{}; assert(runtime.GetAdapterRequest(i, &request));
            if (request.type == type && (token == 0 || request.token == token) &&
                (status == 0 || request.sip_status == status)) ++found;
        }
        return found;
    }
    bool Live(voip::CallHandle call) const {
        voip::CallSnapshot snapshot{};
        return runtime.GetCallSnapshot(call, &snapshot) == voip::Error::ok;
    }
    unsigned Terminals(voip::CallHandle call) const {
        unsigned found = 0;
        for (std::size_t i = 0; i < count; ++i)
            if (events[i].type == voip::EventType::call_state &&
                events[i].call.slot == call.slot && events[i].call.generation == call.generation &&
                (events[i].destination_state == voip::CallState::terminated ||
                 events[i].destination_state == voip::CallState::idle)) ++found;
        return found;
    }
};

void test_task5_command_state_matrix() {
    using E = voip::Error;
    enum State { queued_outgoing, queued_incoming, incoming, early, established, held, disconnecting, stale };
    // Columns: answer, reject, cancel, hangup, hold, resume. These follow the
    // scheduler's termination policy; public Cancel is outgoing-only.
    const E matrix[8][6] = {
        {E::invalid_state, E::invalid_state, E::ok, E::invalid_state, E::invalid_state, E::invalid_state},
        {E::ok, E::ok, E::invalid_state, E::invalid_state, E::invalid_state, E::invalid_state},
        {E::ok, E::ok, E::invalid_state, E::invalid_state, E::invalid_state, E::invalid_state},
        {E::invalid_state, E::invalid_state, E::ok, E::invalid_state, E::invalid_state, E::invalid_state},
        {E::invalid_state, E::invalid_state, E::invalid_state, E::ok, E::ok, E::invalid_state},
        {E::invalid_state, E::invalid_state, E::invalid_state, E::ok, E::invalid_state, E::ok},
        {E::invalid_state, E::invalid_state, E::invalid_state, E::invalid_state, E::invalid_state, E::invalid_state},
        {E::invalid_handle, E::invalid_handle, E::invalid_handle, E::invalid_handle, E::invalid_handle, E::invalid_handle},
    };
    for (unsigned state = 0; state < 8; ++state) for (unsigned command = 0; command < 6; ++command) {
        ControlFixture f;
        if (state <= queued_incoming) (void)f.Incoming(700);
        const bool outgoing = state == queued_outgoing || state == early;
        auto call = outgoing ? f.Outgoing() : f.Incoming(701);
        if (state == established || state == held)
            f.Notify(voip::RuntimeNotification::Type::call_accepted, 701);
        if (state == held) {
            voip::OperationId setup{};
            assert(f.runtime.SetHeld(call,true,&setup)==E::ok); f.Step(f.now);
            f.Notify(voip::RuntimeNotification::Type::call_held, 701);
        }
        if (state == disconnecting) f.Step(2000); // answer timeout; teardown deferred
        if (state == stale) { f.Notify(voip::RuntimeNotification::Type::call_finished, 701); assert(!f.Live(call)); }
        const auto before = f.runtime.AdapterRequestCount();
        voip::OperationId operation{};
        E admitted = E::internal_failure;
        switch (command) {
        case 0: admitted = f.runtime.Answer(call, &operation); break;
        case 1: admitted = f.runtime.Reject(call, 486, &operation); break;
        case 2: admitted = f.runtime.Cancel(call, &operation); break;
        case 3: admitted = f.runtime.Hangup(call, &operation); break;
        case 4: admitted = f.runtime.SetHeld(call, true, &operation); break;
        case 5: admitted = f.runtime.SetHeld(call, false, &operation); break;
        }
        if (state == stale) { assert(admitted == E::invalid_handle); continue; }
        assert(admitted == E::ok); f.Step(f.now);
        const bool media_pending = matrix[state][command] == E::ok && command >= 4;
        if (f.Completions(operation, matrix[state][command]) != (media_pending ? 0u : 1u)) {
            std::fprintf(stderr, "matrix state=%u command=%u missing completion\n", state, command);
            assert(false);
        }
        if (matrix[state][command] != E::ok || state == queued_outgoing ||
            (state == queued_incoming && command == 0))
            assert(f.runtime.AdapterRequestCount() == before);
        if (media_pending) {
            // A second non-terminal command reports busy rather than
            // cancelling the in-flight media negotiation. Hangup is excluded
            // here because `established` and `hold(media)` have no legal
            // `timeout` edge in state_machine.uml, so `finish` is the only
            // exit if PJSUA never reports the negotiation result; that escape
            // is proven by
            // test_final_hangup_escapes_unresolved_media_control.
            voip::OperationId second{};
            assert(f.runtime.SetHeld(call, command == 4, &second) == E::ok);
            f.Step(f.now);
            assert(f.Completions(second, E::busy) == 1);
            assert(f.Completions(operation, E::ok) == 0);
        }
        f.runtime.DrainAdapterCallbacks(); f.Step(f.now); f.Step(f.now);
        assert(f.Completions(operation, matrix[state][command]) == 1);
    }
}

void test_task5_preanswer_and_terminal_acceptance() {
    ControlFixture f;
    const auto active = f.Incoming(710);
    const auto queued = f.Incoming(711);
    voip::OperationId answer{};
    assert(f.runtime.Answer(queued, &answer) == voip::Error::ok); f.Step(f.now);
    assert(f.Completions(answer, voip::Error::ok) == 1);
    assert(f.Requests(voip::RuntimeRequest::Type::answer) == 0);
    f.Notify(voip::RuntimeNotification::Type::call_finished, 710);
    assert(!f.Live(active) && f.Terminals(active) == 1);
    assert(f.Requests(voip::RuntimeRequest::Type::answer, 711) == 1);
    assert(f.Completions(answer, voip::Error::ok) == 1);
    f.Notify(voip::RuntimeNotification::Type::call_accepted, 711);
    voip::OperationId hangup{};
    assert(f.runtime.Hangup(queued, &hangup) == voip::Error::ok); f.Step(f.now);
    assert(f.Completions(hangup, voip::Error::ok) == 1);
    assert(f.Live(queued));
    assert(f.runtime.GetResourceSnapshot().available_promoted_calls == 1);
    f.runtime.DrainAdapterCallbacks(); f.Step(f.now); f.Step(f.now);
    assert(!f.Live(queued) && f.Terminals(queued) == 1);
    assert(f.Completions(hangup, voip::Error::ok) == 1);
    ControlFixture pending;
    (void)pending.Incoming(712);
    const auto rejecting = pending.Incoming(713);
    voip::OperationId reject{};
    assert(pending.runtime.Reject(rejecting, 486, &reject) == voip::Error::ok);
    pending.Step(pending.now);
    assert(pending.Completions(reject, voip::Error::ok) == 1);
    pending.Notify(voip::RuntimeNotification::Type::call_finished, 712);
    assert(pending.Requests(voip::RuntimeRequest::Type::promote_incoming, 713) == 0);
    // The logical terminal event is committed at native acceptance, but the
    // native callback still owns this context's eventual lease release.
    assert(pending.Live(rejecting) && pending.Terminals(rejecting) == 1);
    pending.runtime.DrainAdapterCallbacks(); pending.Step(pending.now);
    assert(!pending.Live(rejecting) && pending.Terminals(rejecting) == 1);
}

void test_task5_independent_timer_epochs_and_wrap() {
    for (bool incoming : {false, true}) {
        ControlFixture f(500, 100);
        (void)f.Incoming(720);
        const auto waiting = incoming ? f.Incoming(721) : f.Outgoing();
        f.Step(1099); assert(f.Live(waiting));
        f.Notify(voip::RuntimeNotification::Type::call_finished, 720);
        const unsigned teardowns = f.Requests(voip::RuntimeRequest::Type::teardown);
        f.Step(1100); assert(f.Live(waiting) && f.Terminals(waiting) == 0);
        f.Step(1198); assert(f.Terminals(waiting) == 0);
        f.Step(1199); assert(f.Terminals(waiting) == 1 && f.Live(waiting));
        assert(f.Requests(voip::RuntimeRequest::Type::teardown) == teardowns + 1);
        f.runtime.DrainAdapterCallbacks(); f.Step(1199);
        assert(!f.Live(waiting));
    }
    for (bool queued : {false, true}) {
        ControlFixture f(100, 1000);
        f.Step(UINT64_MAX - 49);
        if (queued) (void)f.Incoming(722);
        const auto call = f.Outgoing();
        f.Step(UINT64_MAX); assert(f.Terminals(call) == 0);
        f.Step(49); assert(f.Terminals(call) == 0);
        if (queued) { f.Step(50); assert(f.Terminals(call) == 1); }
        else { f.Step(949); assert(f.Terminals(call) == 0); f.Step(950); assert(f.Terminals(call) == 1); }
    }
}

void test_full_fifo_reports_queue_full_before_terminal_reservation() {
    ControlFixture f;
    (void)f.Incoming(900);
    voip::AgentHandle second{};
    assert(f.runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    voip::OperationId operation{};
    assert(f.runtime.Dial(second, {"sip:second@example.test"}, &operation) == voip::Error::ok);
    f.Step(f.now);
    for (unsigned i = 0; i < 5; ++i) (void)f.Incoming(901 + i);
    const auto before = f.runtime.GetResourceSnapshot();
    assert(before.promoted_calls == 2 && before.queued_calls == 5);
    const auto requests = f.runtime.AdapterRequestCount();
    assert(f.runtime.Dial(f.agent, {"sip:overflow@example.test"}, &operation) == voip::Error::ok);
    f.Step(f.now);
    assert(f.Completions(operation, voip::Error::queue_full) == 1);
    assert(f.runtime.AdapterRequestCount() == requests);
    const auto after = f.runtime.GetResourceSnapshot();
    assert(after.active_calls == 7 && after.promoted_calls == 2 && after.queued_calls == 5);
    assert(after.available_operations == before.available_operations);
}

void test_task5_queue_timeout_and_media_terminal_failure() {
    for (bool incoming : {false, true}) {
        ControlFixture f(100, 1000);
        (void)f.Incoming(730);
        auto call = incoming ? f.Incoming(731) : f.Outgoing();
        const auto requests = f.runtime.AdapterRequestCount();
        f.Step(1099); assert(f.Live(call) && f.Terminals(call) == 0);
        f.Step(1100); assert(f.Terminals(call) == 1);
        if (incoming) {
            assert(f.Requests(voip::RuntimeRequest::Type::reject, 731, 480) == 1);
            assert(f.Live(call)); // native callback still owns this context
            f.runtime.DrainAdapterCallbacks(); f.Step(1100);
        } else assert(f.runtime.AdapterRequestCount() == requests);
        assert(!f.Live(call));
        assert(f.Terminals(call) == 1);
    }
    ControlFixture f;
    auto call = f.Incoming(732);
    f.Notify(voip::RuntimeNotification::Type::call_accepted, 732);
    voip::OperationId hold{};
    assert(f.runtime.SetHeld(call, true, &hold) == voip::Error::ok); f.Step(f.now);
    assert(f.Completions(hold, voip::Error::cancelled) == 0);
    f.Notify(voip::RuntimeNotification::Type::call_finished, 732);
    assert(f.Completions(hold, voip::Error::cancelled) == 1);
    assert(!f.Live(call) && f.Terminals(call) == 1);
    f.runtime.DrainAdapterCallbacks(); f.Step(f.now);
    assert(f.Completions(hold, voip::Error::cancelled) == 1);
}

void test_task6_cancel_retains_native_until_finish() {
    ControlFixture f;
    const auto first = f.Incoming(800);
    const auto cancelled = f.Incoming(801);
    const auto next = f.Incoming(802);
    voip::OperationId answer{};
    assert(f.runtime.Answer(cancelled, &answer) == voip::Error::ok);
    f.Step(f.now);
    assert(f.Completions(answer, voip::Error::ok) == 1);
    voip::RuntimeNotification rejected{};
    rejected.type = voip::RuntimeNotification::Type::call_rejected;
    rejected.token = 801;
    rejected.agent = f.agent;
    rejected.error = voip::Error::cancelled;
    rejected.sip_status = 487;
    rejected.status = {voip::Error::cancelled, 487, {}};
    std::strcpy(rejected.status.reason, "Request Terminated");
    std::strcpy(rejected.remote_address, "192.0.2.81");
    const auto terminal_before = f.count;
    assert(f.runtime.InjectNotification(rejected) == voip::Error::ok);
    f.Step(f.now);
    assert(f.Live(cancelled)); // rejection is not native quiescence
    assert(f.Terminals(cancelled) == 1);
    bool copied_terminal = false;
    for (std::size_t i = terminal_before; i < f.count; ++i) {
        const auto &event = f.events[i];
        if (event.type != voip::EventType::call_state ||
            event.call.slot != cancelled.slot ||
            event.call.generation != cancelled.generation ||
            event.destination_state != voip::CallState::terminated)
            continue;
        assert(event.status.error == voip::Error::cancelled);
        assert(event.status.sip_status == 487);
        assert(std::strcmp(event.status.reason, "Request Terminated") == 0);
        assert(event.call_snapshot.sip_status == 487);
        assert(std::strcmp(event.call_snapshot.remote_address, "192.0.2.81") == 0);
        copied_terminal = true;
    }
    assert(copied_terminal);
    f.Notify(voip::RuntimeNotification::Type::call_rejected, 801);
    assert(f.Live(cancelled) && f.Terminals(cancelled) == 1);
    f.Notify(voip::RuntimeNotification::Type::call_finished, 800);
    assert(!f.Live(first));
    assert(f.Requests(voip::RuntimeRequest::Type::answer, 801) == 0);
    assert(f.Requests(voip::RuntimeRequest::Type::promote_incoming, 802) == 1);
    assert(f.Live(next));
    f.Notify(voip::RuntimeNotification::Type::call_finished, 801);
    assert(!f.Live(cancelled) && f.Terminals(cancelled) == 1);
    assert(f.Completions(answer, voip::Error::ok) == 1);
}

void test_task6_media_results_complete_once() {
    ControlFixture f;
    const auto call=f.Incoming(810);
    f.Notify(voip::RuntimeNotification::Type::call_accepted,810);
    for (bool held : {true,false}) {
        for (bool failure : {true,false}) {
            voip::OperationId operation{};
            assert(f.runtime.SetHeld(call,held,&operation)==voip::Error::ok); f.Step(f.now);
            voip::RuntimeNotification n{};
            n.token=810;
            n.type=held ? voip::RuntimeNotification::Type::call_held : voip::RuntimeNotification::Type::call_resumed;
            n.error=failure ? voip::Error::negotiation_failed : voip::Error::ok;
            n.sip_status=failure ? 488 : 200;
            n.status.sip_status=n.sip_status;
            std::strcpy(n.status.reason, failure ? "Not Acceptable Here" : "OK");
            assert(f.runtime.InjectNotification(n)==voip::Error::ok); f.Step(f.now);
            assert(f.Completions(operation,n.error)==1);
            for (unsigned i=0; i<f.count; ++i) {
                if (f.events[i].type==voip::EventType::operation_terminal &&
                    f.events[i].operation==operation) {
                    assert(f.events[i].status.sip_status==n.sip_status);
                    assert(std::strcmp(f.events[i].status.reason,n.status.reason)==0);
                }
            }
            voip::CallSnapshot state{}; assert(f.runtime.GetCallSnapshot(call,&state)==voip::Error::ok);
            assert(state.state==((held != failure) ? voip::CallState::hold : voip::CallState::established));
            const auto before=f.count;
            assert(f.runtime.InjectNotification(n)==voip::Error::ok); f.Step(f.now);
            assert(f.count==before && f.Completions(operation,n.error)==1);
        }
    }
}

void test_task6_queued_finish_only_failure_preserves_status_and_fifo() {
    ControlFixture incoming;
    const auto active=incoming.Incoming(830);
    const auto failed=incoming.Incoming(831);
    const auto next=incoming.Incoming(832);
    voip::OperationId answer{};
    assert(incoming.runtime.Answer(failed,&answer)==voip::Error::ok);
    incoming.Step(incoming.now);
    assert(incoming.Completions(answer,voip::Error::ok)==1);
    assert(incoming.Requests(voip::RuntimeRequest::Type::answer,831)==0);
    voip::RuntimeNotification finish{};
    finish.type=voip::RuntimeNotification::Type::call_finished;
    finish.token=831;
    finish.error=voip::Error::signaling_failed;
    finish.status.error=voip::Error::signaling_failed;
    std::strcpy(finish.status.reason,"Transport failure");
    const auto before=incoming.count;
    assert(incoming.runtime.InjectNotification(finish)==voip::Error::ok);
    incoming.Step(incoming.now);
    assert(!incoming.Live(failed) && incoming.Terminals(failed)==1);
    assert(incoming.Completions(answer,voip::Error::ok)==1);
    assert(incoming.Requests(voip::RuntimeRequest::Type::promote_incoming,832)==0);
    bool copied_terminal=false;
    for (std::size_t i=before;i<incoming.count;++i) {
        const auto &event=incoming.events[i];
        if (event.type!=voip::EventType::call_state ||
            event.call.slot!=failed.slot ||
            event.call.generation!=failed.generation ||
            event.destination_state!=voip::CallState::terminated) continue;
        assert(event.status.error==voip::Error::signaling_failed);
        assert(event.status.sip_status==0);
        assert(std::strcmp(event.status.reason,"Transport failure")==0);
        copied_terminal=true;
    }
    assert(copied_terminal);
    const auto after_finish=incoming.count;
    assert(incoming.runtime.InjectNotification(finish)==voip::Error::ok);
    incoming.Step(incoming.now);
    assert(incoming.count==after_finish && incoming.Terminals(failed)==1);
    assert(incoming.Completions(answer,voip::Error::ok)==1);
    incoming.Notify(voip::RuntimeNotification::Type::call_finished,830);
    assert(!incoming.Live(active));
    assert(incoming.Requests(voip::RuntimeRequest::Type::promote_incoming,832)==1);
    assert(incoming.Live(next));
}

void test_task6_finish_only_failure_completes_pending_operation_once() {
    ControlFixture pending;
    const auto established=pending.Incoming(840);
    pending.Notify(voip::RuntimeNotification::Type::call_accepted,840);
    voip::OperationId hold{};
    assert(pending.runtime.SetHeld(established,true,&hold)==voip::Error::ok);
    pending.Step(pending.now);
    assert(pending.Completions(hold,voip::Error::media_failed)==0);
    voip::RuntimeNotification finish{};
    finish.type=voip::RuntimeNotification::Type::call_finished;
    finish.token=840;
    finish.error=voip::Error::media_failed;
    finish.status.error=voip::Error::media_failed;
    std::strcpy(finish.status.reason,"Media failure");
    const auto operation_before=pending.count;
    assert(pending.runtime.InjectNotification(finish)==voip::Error::ok);
    pending.Step(pending.now);
    assert(!pending.Live(established) && pending.Terminals(established)==1);
    assert(pending.Completions(hold,voip::Error::media_failed)==1);
    std::size_t terminal_index=pending.count;
    std::size_t operation_index=pending.count;
    for (std::size_t i=operation_before;i<pending.count;++i) {
        const auto &event=pending.events[i];
        if (event.type==voip::EventType::call_state &&
            event.call.slot==established.slot &&
            event.call.generation==established.generation &&
            event.destination_state==voip::CallState::terminated) {
            assert(event.status.error==voip::Error::media_failed);
            assert(std::strcmp(event.status.reason,"Media failure")==0);
            terminal_index=i;
        } else if (event.type==voip::EventType::operation_terminal &&
                   event.operation==hold) {
            assert(event.status.error==voip::Error::media_failed);
            operation_index=i;
        }
    }
    assert(terminal_index<operation_index && operation_index<pending.count);
    const auto after_operation=pending.count;
    assert(pending.runtime.InjectNotification(finish)==voip::Error::ok);
    pending.Step(pending.now);
    assert(pending.count==after_operation);
    assert(pending.Completions(hold,voip::Error::media_failed)==1);
}

void test_task6_cancel_before_promotion_releases_only_on_finish() {
    ControlFixture f;
    (void)f.Incoming(820);
    const auto cancelled=f.Incoming(821);
    voip::OperationId answer{};
    assert(f.runtime.Answer(cancelled,&answer)==voip::Error::ok); f.Step(f.now);
    // Native CANCEL was observed inside the same pump that freed the lease;
    // its copied terminal notification has not reached core yet.
    f.runtime.FailNextAdapter(voip::RuntimeRequest::Type::promote_incoming,voip::Error::invalid_handle);
    f.Notify(voip::RuntimeNotification::Type::call_finished,820);
    assert(f.Live(cancelled));
    assert(f.Requests(voip::RuntimeRequest::Type::answer,821)==0);
    f.Notify(voip::RuntimeNotification::Type::call_rejected,821);
    assert(f.Live(cancelled));
    f.Notify(voip::RuntimeNotification::Type::call_finished,821);
    assert(!f.Live(cancelled) && f.Terminals(cancelled)==1);
    assert(f.Completions(answer,voip::Error::ok)==1);
}


// Final-review regression: a later native notification that carries no remote
// address must not erase the address copied at admission. `remote_uri` was
// already guarded; `remote_address` was overwritten unconditionally, so the
// first follow-up callback blanked a field the public snapshot promises.
void test_final_copied_remote_address_survives_later_notifications() {
    ControlFixture f;
    voip::RuntimeNotification incoming{};
    incoming.type = voip::RuntimeNotification::Type::incoming_call;
    incoming.token = 980;
    incoming.agent = f.agent;
    std::strcpy(incoming.remote_uri, "sip:caller@example.test");
    std::strcpy(incoming.remote_address, "198.51.100.7");
    const auto before = f.count;
    assert(f.runtime.InjectNotification(incoming) == voip::Error::ok);
    f.Step(f.now);
    voip::CallHandle call{};
    for (auto i = before; i < f.count; ++i)
        if (f.events[i].type == voip::EventType::incoming_call) call = f.events[i].call;
    assert(call.IsValid());
    voip::CallSnapshot admitted{};
    assert(f.runtime.GetCallSnapshot(call, &admitted) == voip::Error::ok);
    assert(std::strcmp(admitted.remote_uri, "sip:caller@example.test") == 0);
    assert(std::strcmp(admitted.remote_address, "198.51.100.7") == 0);
    voip::RuntimeNotification accepted{};
    accepted.type = voip::RuntimeNotification::Type::call_accepted;
    accepted.token = 980;
    accepted.agent = f.agent;
    assert(f.runtime.InjectNotification(accepted) == voip::Error::ok);
    f.Step(f.now);
    voip::CallSnapshot established{};
    assert(f.runtime.GetCallSnapshot(call, &established) == voip::Error::ok);
    assert(std::strcmp(established.remote_uri, "sip:caller@example.test") == 0);
    assert(std::strcmp(established.remote_address, "198.51.100.7") == 0);
}

// Final-review regression: PJSUA gives no media callback at all when a remote
// peer rejects a hold/resume re-INVITE without terminating the dialog
// (`pjsua_call_on_media_update` skips `on_call_media_state` for a failed
// negotiation while CONFIRMED). The pending media operation therefore never
// resolves, so a terminal control must still be accepted or the call and one
// of the two global promoted leases are wedged until remote BYE or shutdown.
void test_final_hangup_escapes_unresolved_media_control() {
    for (bool held : {true, false}) {
        ControlFixture f;
        const auto call = f.Incoming(981);
        f.Notify(voip::RuntimeNotification::Type::call_accepted, 981);
        if (!held) {
            voip::OperationId settle{};
            assert(f.runtime.SetHeld(call, true, &settle) == voip::Error::ok);
            f.Step(f.now);
            f.Notify(voip::RuntimeNotification::Type::call_held, 981);
            assert(f.Completions(settle, voip::Error::ok) == 1);
        }
        voip::OperationId media{};
        assert(f.runtime.SetHeld(call, held, &media) == voip::Error::ok);
        f.Step(f.now);
        assert(f.Completions(media, voip::Error::cancelled) == 0);
        // A non-terminal second command still reports busy; only termination
        // may supersede an unresolved native media negotiation.
        voip::OperationId second{};
        assert(f.runtime.SetHeld(call, !held, &second) == voip::Error::ok);
        f.Step(f.now);
        assert(f.Completions(second, voip::Error::busy) == 1);
        voip::OperationId hangup{};
        assert(f.runtime.Hangup(call, &hangup) == voip::Error::ok);
        f.Step(f.now);
        assert(f.Completions(media, voip::Error::cancelled) == 1);
        assert(f.Requests(voip::RuntimeRequest::Type::hangup, 981) == 1);
        f.Notify(voip::RuntimeNotification::Type::call_finished, 981);
        assert(!f.Live(call));
        assert(f.Terminals(call) == 1);
        assert(f.Completions(media, voip::Error::cancelled) == 1);
        assert(f.Completions(hangup, voip::Error::ok) == 1);
        const auto after = f.runtime.GetResourceSnapshot();
        assert(after.active_calls == 0 && after.available_promoted_calls == 2);
    }
}

// Final review asked whether the FIFO-full dial preflight, which runs before
// scheduler admission, can mask a malformed URI as a retryable `queue_full`.
// It cannot: `VoipRuntime::Dial` validates the URI synchronously and refuses
// it before a command or operation is ever reserved. This pins that ordering
// so the preflight is never moved ahead of validation.
void test_final_full_fifo_still_reports_invalid_dial_arguments() {
    ControlFixture f;
    (void)f.Incoming(990);
    voip::AgentHandle second{};
    assert(f.runtime.GetAgentHandle(1, &second) == voip::Error::ok);
    voip::OperationId operation{};
    assert(f.runtime.Dial(second, {"sip:second@example.test"}, &operation) == voip::Error::ok);
    f.Step(f.now);
    for (unsigned i = 0; i < 5; ++i) (void)f.Incoming(991 + i);
    const auto before = f.runtime.GetResourceSnapshot();
    assert(before.promoted_calls == 2 && before.queued_calls == 5);
    const auto requests = f.runtime.AdapterRequestCount();
    voip::OperationId malformed = 7;
    assert(f.runtime.Dial(f.agent, {""}, &malformed) == voip::Error::invalid_argument);
    assert(malformed == 0);
    char overlong[voip::max_uri_length + 2];
    std::memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';
    assert(f.runtime.Dial(f.agent, {overlong}, &malformed) == voip::Error::invalid_argument);
    f.Step(f.now);
    assert(f.runtime.GetResourceSnapshot().available_operations ==
           before.available_operations);
    voip::OperationId overflow{};
    assert(f.runtime.Dial(f.agent, {"sip:overflow@example.test"}, &overflow) == voip::Error::ok);
    f.Step(f.now);
    assert(f.Completions(overflow, voip::Error::queue_full) == 1);
    assert(f.runtime.AdapterRequestCount() == requests);
    const auto after = f.runtime.GetResourceSnapshot();
    assert(after.active_calls == 7 && after.promoted_calls == 2 && after.queued_calls == 5);
}

} // namespace

int main(int argc, char **argv) {
    test_full_fifo_reports_queue_full_before_terminal_reservation();
    if (argc == 2) {
        if (std::strcmp(argv[1], "--final") == 0) {
            test_final_copied_remote_address_survives_later_notifications();
            test_final_hangup_escapes_unresolved_media_control();
            test_final_full_fifo_still_reports_invalid_dial_arguments();
            std::puts("VoipServiceCoreTest focused PASSED");
            return 0;
        }
        if (std::strcmp(argv[1], "--task6") == 0) { test_task6_cancel_retains_native_until_finish(); test_task6_media_results_complete_once(); test_task6_queued_finish_only_failure_preserves_status_and_fifo(); test_task6_finish_only_failure_completes_pending_operation_once(); test_task6_cancel_before_promotion_releases_only_on_finish(); }
        else if (std::strcmp(argv[1], "--task5-matrix") == 0) test_task5_command_state_matrix();
        else if (std::strcmp(argv[1], "--task5-preanswer") == 0) test_task5_preanswer_and_terminal_acceptance();
        else if (std::strcmp(argv[1], "--task5-timers") == 0) test_task5_independent_timer_epochs_and_wrap();
        else if (std::strcmp(argv[1], "--task5-timeout") == 0) test_task5_queue_timeout_and_media_terminal_failure();
        else return 2;
        std::puts("VoipServiceCoreTest focused PASSED");
        return 0;
    }
    test_task6_cancel_retains_native_until_finish();
    test_task6_media_results_complete_once();
    test_task6_queued_finish_only_failure_preserves_status_and_fifo();
    test_task6_finish_only_failure_completes_pending_operation_once();
    test_task6_cancel_before_promotion_releases_only_on_finish();
    test_final_copied_remote_address_survives_later_notifications();
    test_final_hangup_escapes_unresolved_media_control();
    test_final_full_fifo_still_reports_invalid_dial_arguments();
    test_task5_command_state_matrix();
    test_task5_preanswer_and_terminal_acceptance();
    test_task5_independent_timer_epochs_and_wrap();
    test_task5_queue_timeout_and_media_terminal_failure();
    test_composes_five_agents_and_bounded_scheduler();
    test_security_rejected_and_zero_wait_is_nonblocking();
    test_trace_resources_leases_and_repeated_lifecycles();
    test_every_accepted_operation_gets_terminal_event();
    test_incoming_notification_is_admitted_and_rejected();
    test_queued_incoming_rejection_releases_without_use_after_free();
    test_queued_call_times_out_and_publishes_terminal_operation();
    test_hangup_finishes_promoted_call_and_preserves_terminal_trace();
    test_fake_adapter_records_bounded_copied_requests();
    test_outgoing_dial_admission_copies_uri_and_completes_at_durability();
    test_failed_first_effect_does_not_skip_second_in_single_capacity_pass();
    test_disabled_registration_rejects_outgoing_admission();
    test_failed_hold_preserves_established_state();
    test_shutdown_adapter_failure_preserves_reachable_runtime();
    test_reinitialize_requires_draining_prior_events();
    test_initialization_failure_rolls_back_adapter_and_events();
    test_initialize_waits_for_actor_bootstrap_without_holding_runtime_mutex();
    test_bootstrap_drains_at_most_one_notification_capacity();
    test_registration_notifications_update_only_the_matching_agent();
    test_invalid_promoted_answer_does_not_call_adapter();
    test_queued_incoming_reject_uses_native_callback_before_release();
    test_stale_agent_is_rejected_before_operation_or_mailbox_reservation();
    test_queued_terminal_event_commits_before_call_release();
    test_teardown_failure_retries_and_releases_call_capacity();
    test_failed_preanswered_promotion_rolls_back_all_leases();
    test_failed_promotion_commits_terminal_before_release();
    test_shutdown_waits_for_existing_teardown_callback();
    test_shutdown_waits_for_user_native_terminal_requests();
    test_concurrent_shutdown_callers_are_serialized();
    test_initialize_waits_for_shutdown_coordination();
    test_fake_callback_state_resets_between_lifecycles();
    test_shutdown_timeout_requires_retry_before_reinitialize();
    test_shutdown_orders_native_reject_teardown_before_adapter_shutdown();
    test_prepublication_failures_abort_incoming_and_restore_reservations();
    test_incoming_admission_orders_native_180_and_hides_failure();
    std::puts("VoipServiceCoreTest PASSED");
    return 0;
}
