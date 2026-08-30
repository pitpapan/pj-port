#include <voip/VoipService.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <array>
#include <cstdint>

static_assert(CONFIG_VOIP_MAX_AGENTS == 5);
static_assert(CONFIG_VOIP_MAX_LOGICAL_CALLS == 7);
static_assert(CONFIG_VOIP_MAX_PROMOTED_CALLS == 2);
static_assert(CONFIG_VOIP_PENDING_CALL_CAPACITY == 5);
static_assert(CONFIG_VOIP_COMMAND_CAPACITY == 16);
static_assert(CONFIG_VOIP_OPERATION_CAPACITY == 16);
static_assert(CONFIG_VOIP_EVENT_CAPACITY == 32);

namespace {

class SentinelSource final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

class SentinelSink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
    void Flush() noexcept override {}
};

voip::ServiceConfig MakeConfig(std::array<voip::AgentConfig, 5> &agents,
                               std::array<SentinelSource, 5> &sources,
                               std::array<SentinelSink, 5> &sinks) {
    static constexpr const char *const identities[5] = {
        "sip:core0@example.test", "sip:core1@example.test",
        "sip:core2@example.test", "sip:core3@example.test",
        "sip:core4@example.test",
    };
    for (std::size_t i = 0; i < agents.size(); ++i) {
        agents[i] = {{identities[i], "sip:example.test", "core", "secret"},
                     {&sources[i], &sinks[i]}, true};
    }
    return {agents.data(), static_cast<std::uint8_t>(agents.size()), 1000, 1000,
            {8000, 160, 1, voip::SampleFormat::signed_16},
            {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
}

bool GetCallEvent(voip::VoipService &service, voip::OperationId operation,
                  voip::CallHandle *call) {
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::call_state &&
            event.operation == operation) {
            *call = event.call;
            return true;
        }
    }
    return false;
}

bool WaitForOperation(voip::VoipService &service, voip::OperationId operation,
                      voip::Error expected) {
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::operation_terminal &&
            event.operation == operation)
            return event.status.error == expected;
    }
    return false;
}

bool WaitForTimedOut(voip::VoipService &service, voip::OperationId operation) {
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 150; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::operation_terminal &&
            event.operation == operation)
            return event.status.error == voip::Error::timed_out;
    }
    return false;
}

bool WaitForEstablished(voip::VoipService &service, voip::CallHandle call) {
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::call_state &&
            event.call.slot == call.slot &&
            event.call.generation == call.generation &&
            event.destination_state == voip::CallState::established)
            return true;
        voip::CallSnapshot snapshot{};
        if (service.GetCallSnapshot(call, &snapshot) == voip::Error::ok &&
            snapshot.state == voip::CallState::established)
            return true;
    }
    return false;
}

bool RunProof() {
    std::array<SentinelSource, 5> sources;
    std::array<SentinelSink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = MakeConfig(agents, sources, sinks);
    static voip::VoipService service;
    if (service.Initialize(config) != voip::Error::ok) return false;

    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < handles.size(); ++i)
        if (service.GetAgentHandle(i, &handles[i]) != voip::Error::ok)
            return false;

    // The first two calls occupy distinct agents. The next five are ordered
    // so the agent-0 head is blocked by call 0, proving strict HOL behavior.
    const std::array<std::uint8_t, 7> call_agents = {0, 1, 0, 2, 3, 4, 1};
    std::array<voip::CallHandle, 7> calls{};
    std::array<voip::OperationId, 7> dial_operations{};
    for (std::size_t i = 0; i < calls.size(); ++i) {
        voip::OperationId &operation = dial_operations[i];
        if (service.Dial(handles[call_agents[i]], {"sip:peer@example.test"},
                         &operation) != voip::Error::ok ||
            !GetCallEvent(service, operation, &calls[i]))
            return false;
    }

    voip::ResourceSnapshot full = service.GetResourceSnapshot();
    if (full.active_calls != 7 || full.promoted_calls != 2 ||
        full.queued_calls != 5 || full.available_fifo_entries != 0 ||
        full.available_logical_calls != 0 || full.available_promoted_calls != 0)
        return false;

    // Releasing agent 1 leaves one global slot idle, but the agent-0 FIFO head
    // remains blocked. A later eligible entry must not bypass that head.
    voip::OperationId cancel = 0;
    if (service.Cancel(calls[1], &cancel) != voip::Error::ok ||
        !WaitForOperation(service, cancel, voip::Error::cancelled) ||
        service.GetResourceSnapshot().promoted_calls != 1 ||
        service.GetResourceSnapshot().queued_calls != 5 ||
        service.GetResourceSnapshot().available_promoted_calls != 1)
        return false;
    voip::CallSnapshot head{};
    voip::CallSnapshot behind_head{};
    if (service.GetCallSnapshot(calls[2], &head) != voip::Error::ok ||
        head.state != voip::CallState::hold ||
        head.hold_reason != voip::HoldReason::waiting ||
        service.GetCallSnapshot(calls[3], &behind_head) != voip::Error::ok ||
        behind_head.state != voip::CallState::hold ||
        behind_head.hold_reason != voip::HoldReason::waiting)
        return false;

    // Releasing agent 0 makes the head eligible; both available global slots
    // are then consumed by the head and the next eligible FIFO entry.
    if (service.Cancel(calls[0], &cancel) != voip::Error::ok ||
        !WaitForOperation(service, cancel, voip::Error::cancelled) ||
        !WaitForEstablished(service, calls[2]) ||
        !WaitForEstablished(service, calls[3]))
        return false;
    voip::ResourceSnapshot after_heads = service.GetResourceSnapshot();
    if (after_heads.promoted_calls != 2 || after_heads.queued_calls != 3)
        return false;

    // Cancel the promoted head; call 4 advances, leaving call 5 queued as the
    // deterministic timeout witness before shutdown.
    if (service.Cancel(calls[2], &cancel) != voip::Error::ok ||
        !WaitForOperation(service, cancel, voip::Error::cancelled))
        return false;
    // Call 4 remains queued after the second cancellation. Its retained dial
    // operation must time out and its public handle must be stale before
    // shutdown begins.
    if (!WaitForTimedOut(service, dial_operations[5])) return false;
    voip::CallSnapshot timed_out{};
    if (service.GetCallSnapshot(calls[5], &timed_out) !=
        voip::Error::invalid_handle)
        return false;
    if (service.Shutdown() != voip::Error::ok) return false;
    if (service.Shutdown() != voip::Error::ok) return false;
    return true;
}

} // namespace

int main() {
    if (RunProof()) {
        printk("VOIP CORE SERVICE RESULT: PASSED\n");
        return 0;
    }
    return 1;
}
