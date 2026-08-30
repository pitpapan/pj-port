#include "../../src/core/CallScheduler.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

class Source final : public voip::PcmSource {
public:
    explicit Source(voip::PcmFormat format) noexcept : format_(format) {}
    voip::PcmFormat Format() const noexcept override { return format_; }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override { return voip::Error::ok; }
private:
    voip::PcmFormat format_;
};

class Sink final : public voip::PcmSink {
public:
    explicit Sink(voip::PcmFormat format) noexcept : format_(format) {}
    voip::PcmFormat Format() const noexcept override { return format_; }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override { return voip::Error::ok; }
    void Flush() noexcept override {}
private:
    voip::PcmFormat format_;
};

struct Fixture {
    Source source{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Sink sink{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    voip::AgentConfig config{{"sip:a@example.test", "sip:example.test", "a", "p"},
                             {&source, &sink}, true};
    voip::ServiceConfig service{&config, 1, 1000, 5000,
        voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16},
        {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
    voip::AgentRegistry registry;
    voip::AgentHandle agent{};
    Fixture() noexcept {
        assert(registry.Initialize(service) == voip::Error::ok);
        assert(registry.GetAgentHandle(0, &agent) == voip::Error::ok);
        voip::AgentContext *context = registry.Resolve(agent);
        assert(context != nullptr);
        context->registration = voip::RegistrationState::registered;
    }
};

void test_immediate_promotion_and_same_agent_fifo() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle first{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:one@example.test", &first) ==
           voip::Error::ok);
    assert(scheduler.IsPromoted(first));
    assert(scheduler.PromotedCount() == 1);

    voip::CallHandle second{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:two@example.test", &second) ==
           voip::Error::ok);
    assert(!scheduler.IsPromoted(second));
    assert(scheduler.Snapshot(second).state == voip::CallState::hold);
    assert(scheduler.Snapshot(second).hold_reason == voip::HoldReason::waiting);
    assert(scheduler.QueuedCount() == 1);
}

void test_hol_blocking_and_repeated_promotion() {
    Source source_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Sink sink_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Fixture fixture;
    voip::AgentConfig configs[2] = {fixture.config,
        {{"sip:b@example.test", "sip:example.test", "b", "p"},
         {&source_b, &sink_b}, true}};
    voip::ServiceConfig service = fixture.service;
    service.agents = configs;
    service.agent_count = 2;
    assert(fixture.registry.Initialize(service) == voip::Error::ok);
    voip::AgentHandle agent_a{}, agent_b{};
    assert(fixture.registry.GetAgentHandle(0, &agent_a) == voip::Error::ok);
    assert(fixture.registry.GetAgentHandle(1, &agent_b) == voip::Error::ok);
    fixture.registry.Resolve(agent_a)->registration = voip::RegistrationState::registered;
    fixture.registry.Resolve(agent_b)->registration = voip::RegistrationState::registered;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle a_active{};
    assert(scheduler.AdmitOutgoing(agent_a, "sip:a1@example.test", &a_active) == voip::Error::ok);
    voip::CallHandle a_head{}, b_later{};
    assert(scheduler.AdmitOutgoing(agent_a, "sip:a2@example.test", &a_head) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(agent_b, "sip:b2@example.test", &b_later) == voip::Error::ok);
    assert(scheduler.QueuedCount() == 2);
    assert(!scheduler.IsPromoted(a_head));
    assert(!scheduler.IsPromoted(b_later));
    assert(scheduler.OnAcceptance(a_active) == voip::Error::ok);
    assert(scheduler.Hangup(a_active) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(a_active) == voip::Error::ok);
    assert(scheduler.IsPromoted(a_head));
    assert(scheduler.IsPromoted(b_later));
}

void test_fifo_capacity_and_middle_cancel() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle active{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:active@example.test", &active) == voip::Error::ok);
    voip::CallHandle queued[5]{};
    for (std::size_t i = 0; i < 5; ++i) {
        char uri[64]{};
        std::snprintf(uri, sizeof(uri), "sip:q%zu@example.test", i);
        assert(scheduler.AdmitOutgoing(fixture.agent, uri, &queued[i]) == voip::Error::ok);
    }
    voip::CallHandle rejected{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:overflow@example.test", &rejected) ==
           voip::Error::queue_full);
    assert(scheduler.Cancel(queued[2]) == voip::Error::ok);
    assert(!scheduler.IsLive(queued[2]));
    assert(scheduler.QueuedCount() == 4);
    assert(scheduler.Cancel(queued[0]) == voip::Error::ok);
    assert(scheduler.QueuedCount() == 3);
}

void test_direct_timeout_publishes_before_invalidation() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle call{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:timeout@example.test", &call) ==
           voip::Error::ok);
    voip::ScheduledTransition terminal{};
    assert(scheduler.OnTimeout(call, &terminal) == voip::Error::ok);
    assert(terminal.transition.after.state == voip::CallState::idle);
    assert(terminal.transition.terminal_event_required);
    assert(terminal.snapshot.handle.slot == call.slot);
    assert(terminal.snapshot.handle.generation == call.generation);
    assert(terminal.handle_invalidated);
    assert(!scheduler.IsLive(call));
}

} // namespace

int main() {
    test_immediate_promotion_and_same_agent_fifo();
    test_hol_blocking_and_repeated_promotion();
    test_fifo_capacity_and_middle_cancel();
    test_direct_timeout_publishes_before_invalidation();
    std::puts("CallSchedulerTest PASSED");
    return 0;
}
