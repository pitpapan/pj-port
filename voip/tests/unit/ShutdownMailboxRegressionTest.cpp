#include <voip/VoipService.hpp>

#include "core/VoipRuntime.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

class Source final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

class Sink final : public voip::PcmSink {
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

voip::ServiceConfig Config(voip::AgentConfig &agent, Source &source,
                           Sink &sink) {
    agent = {{"sip:shutdown@example.test", "sip:example.test", "user", "password"},
             {&source, &sink}, true};
    return {&agent, 1, 60000, 60000, {8000, 160, 1,
                                       voip::SampleFormat::signed_16},
            {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
}

void test_shutdown_does_not_need_mailbox_capacity() {
    Source source;
    Sink sink;
    voip::AgentConfig agent{};
    voip::VoipRuntime runtime;
    runtime.SetActorPaused(true);
    assert(runtime.Initialize(Config(agent, source, sink)) == voip::Error::ok);

    voip::AgentHandle handle{};
    assert(runtime.GetAgentHandle(0, &handle) == voip::Error::ok);
    std::array<voip::OperationId, 16> operations{};
    for (std::size_t i = 0; i < operations.size(); ++i) {
        assert(runtime.Dial(handle, {"sip:queued@example.test"},
                            &operations[i]) == voip::Error::ok);
        assert(operations[i] != 0);
    }
    assert(runtime.Shutdown() == voip::Error::shutdown_timeout);
    voip::OperationId rejected = 0;
    assert(runtime.Dial(handle, {"sip:after-shutdown@example.test"},
                        &rejected) == voip::Error::shutting_down);

    runtime.SetActorPaused(false);
    assert(runtime.Shutdown() == voip::Error::ok);
    assert(runtime.Shutdown() == voip::Error::ok);

    std::array<bool, 16> terminal_seen{};
    unsigned stopped = 0;
    voip::Event event{};
    while (runtime.TryGetEvent(&event) == voip::Error::ok) {
        if (event.type == voip::EventType::operation_terminal) {
            for (std::size_t i = 0; i < operations.size(); ++i)
                if (event.operation == operations[i]) {
                    assert(!terminal_seen[i]);
                    terminal_seen[i] = true;
                    assert(event.status.error == voip::Error::cancelled);
                }
        }
        if (event.type == voip::EventType::service_stopped) ++stopped;
    }
    for (bool seen : terminal_seen) assert(seen);
    assert(stopped == 1);
}

} // namespace

int main() {
    test_shutdown_does_not_need_mailbox_capacity();
    std::puts("ShutdownMailboxRegressionTest PASSED");
    return 0;
}
