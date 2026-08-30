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
    voip::SchedulerEffects effects;
    voip::CallHandle a_active{};
    assert(scheduler.AdmitOutgoing(agent_a, "sip:a1@example.test", &a_active,
                                   nullptr, &effects) == voip::Error::ok);
    effects.Clear();
    voip::CallHandle a_head{}, b_later{};
    assert(scheduler.AdmitOutgoing(agent_a, "sip:a2@example.test", &a_head,
                                   nullptr, &effects) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(agent_b, "sip:b2@example.test", &b_later,
                                   nullptr, &effects) == voip::Error::ok);
    assert(scheduler.QueuedCount() == 2);
    assert(!scheduler.IsPromoted(a_head));
    assert(!scheduler.IsPromoted(b_later));
    assert(scheduler.OnAcceptance(a_active) == voip::Error::ok);
    effects.Clear();
    assert(scheduler.Hangup(a_active, nullptr, &effects) == voip::Error::ok);
    effects.Clear();
    assert(scheduler.OnTeardownComplete(a_active, nullptr, &effects) == voip::Error::ok);
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

void test_registration_and_promoted_acceptance_rules() {
    Fixture fixture;
    fixture.registry.Resolve(fixture.agent)->registration =
        voip::RegistrationState::disabled;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle call{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:not-registered@example.test",
                                   &call) == voip::Error::agent_unavailable);
    fixture.registry.Resolve(fixture.agent)->registration =
        voip::RegistrationState::registered;
    assert(scheduler.AdmitOutgoing(fixture.agent, "", &call) ==
           voip::Error::invalid_argument);
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:one@example.test", &call) ==
           voip::Error::ok);
    voip::CallHandle queued{};
    assert(scheduler.AdmitIncoming(fixture.agent, 42, &queued) == voip::Error::ok);
    assert(scheduler.OnAcceptance(queued) == voip::Error::invalid_state);
    assert(scheduler.Answer(queued) == voip::Error::ok);
}

void test_automatic_promotions_are_returned_as_bounded_effects() {
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
    voip::AgentHandle a{}, b{};
    assert(fixture.registry.GetAgentHandle(0, &a) == voip::Error::ok);
    assert(fixture.registry.GetAgentHandle(1, &b) == voip::Error::ok);
    fixture.registry.Resolve(a)->registration = voip::RegistrationState::registered;
    fixture.registry.Resolve(b)->registration = voip::RegistrationState::registered;
    voip::CallScheduler scheduler(fixture.registry);
    voip::SchedulerEffects effects;
    voip::CallHandle active{};
    assert(scheduler.AdmitOutgoing(a, "sip:a@example.test", &active, nullptr,
                                   &effects) == voip::Error::ok);
    assert(effects.count == 1);
    assert(effects.entries[0].handle.slot == active.slot);
    effects.Clear();
    voip::CallHandle queued_a{}, queued_b{};
    assert(scheduler.AdmitIncoming(a, 10, &queued_a) == voip::Error::ok);
    assert(scheduler.Answer(queued_a) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(b, "sip:b@example.test", &queued_b) ==
           voip::Error::ok);
    assert(scheduler.OnAcceptance(active) == voip::Error::ok);
    assert(scheduler.Hangup(active) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(active, nullptr, &effects) ==
           voip::Error::ok);
    assert(effects.count == 2);
    assert(effects.entries[0].handle.slot == queued_a.slot);
    assert(effects.entries[0].acceptance_applied);
    assert(effects.entries[1].handle.slot == queued_b.slot);
}

void test_runtime_tokens_and_lease_release_order() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle outgoing{};
    voip::SchedulerEffects effects;
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:out@example.test",
                                   &outgoing, nullptr, &effects) == voip::Error::ok);
    assert(scheduler.Resolve(outgoing)->runtime_token == 0);
    assert(scheduler.OnAcceptance(outgoing) == voip::Error::ok);
    voip::ScheduledTransition terminal{};
    assert(scheduler.Hangup(outgoing, &terminal, &effects) == voip::Error::ok);
    assert(scheduler.IsLive(outgoing));
    assert(scheduler.PromotedCount() == 1);
    assert(terminal.transition.after.state == voip::CallState::terminated);
    assert(terminal.transition.terminal_event_required);
    assert(terminal.snapshot.handle.generation == outgoing.generation);
    assert(scheduler.OnTeardownComplete(outgoing, nullptr, &effects) ==
           voip::Error::ok);
    assert(!scheduler.IsLive(outgoing));
    assert(scheduler.PromotedCount() == 0);

    voip::CallHandle incoming{};
    assert(scheduler.AdmitIncoming(fixture.agent, 0x1234u, &incoming) ==
           voip::Error::ok);
    assert(scheduler.Resolve(incoming)->runtime_token == 0x1234u);
    assert(scheduler.Cancel(incoming) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(incoming) == voip::Error::ok);
    assert(!scheduler.IsLive(incoming));
    voip::CallHandle replacement{};
    assert(scheduler.AdmitIncoming(fixture.agent, 0x5678u, &replacement) ==
           voip::Error::ok);
    assert(replacement.slot == incoming.slot);
    assert(replacement.generation != incoming.generation);
}

} // namespace

int main() {
    test_immediate_promotion_and_same_agent_fifo();
    test_hol_blocking_and_repeated_promotion();
    test_fifo_capacity_and_middle_cancel();
    test_direct_timeout_publishes_before_invalidation();
    test_registration_and_promoted_acceptance_rules();
    test_automatic_promotions_are_returned_as_bounded_effects();
    test_runtime_tokens_and_lease_release_order();
    std::puts("CallSchedulerTest PASSED");
    return 0;
}
