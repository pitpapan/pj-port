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
    voip::SchedulerEffects effects;
    voip::CallHandle first{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:one@example.test", &first,
                                   nullptr, effects) ==
           voip::Error::ok);
    assert(scheduler.IsPromoted(first));
    assert(scheduler.PromotedCount() == 1);

    voip::CallHandle second{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:two@example.test", &second,
                                   nullptr, effects) ==
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
                                   nullptr, effects) == voip::Error::ok);
    effects.Clear();
    voip::CallHandle a_head{}, b_later{};
    assert(scheduler.AdmitOutgoing(agent_a, "sip:a2@example.test", &a_head,
                                   nullptr, effects) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(agent_b, "sip:b2@example.test", &b_later,
                                   nullptr, effects) == voip::Error::ok);
    assert(scheduler.QueuedCount() == 2);
    assert(!scheduler.IsPromoted(a_head));
    assert(!scheduler.IsPromoted(b_later));
    assert(scheduler.OnAcceptance(a_active) == voip::Error::ok);
    effects.Clear();
    assert(scheduler.Hangup(a_active, nullptr, effects) == voip::Error::ok);
    effects.Clear();
    assert(scheduler.OnTeardownComplete(a_active, nullptr, effects) == voip::Error::ok);
    assert(scheduler.IsPromoted(a_head));
    assert(scheduler.IsPromoted(b_later));
}

void test_fifo_capacity_and_middle_cancel() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::SchedulerEffects effects;
    voip::CallHandle active{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:active@example.test", &active,
                                   nullptr, effects) == voip::Error::ok);
    voip::CallHandle queued[5]{};
    for (std::size_t i = 0; i < 5; ++i) {
        char uri[64]{};
        std::snprintf(uri, sizeof(uri), "sip:q%zu@example.test", i);
        assert(scheduler.AdmitOutgoing(fixture.agent, uri, &queued[i],
                                       nullptr, effects) == voip::Error::ok);
    }
    voip::CallHandle rejected{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:overflow@example.test", &rejected,
                                   nullptr, effects) ==
           voip::Error::queue_full);
    assert(scheduler.Cancel(queued[2], nullptr, effects) == voip::Error::ok);
    assert(!scheduler.IsLive(queued[2]));
    assert(scheduler.QueuedCount() == 4);
    assert(scheduler.Cancel(queued[0], nullptr, effects) == voip::Error::ok);
    assert(scheduler.QueuedCount() == 3);
}

void test_direct_timeout_publishes_before_invalidation() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::SchedulerEffects effects;
    voip::CallHandle call{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:timeout@example.test", &call,
                                   nullptr, effects) ==
           voip::Error::ok);
    voip::ScheduledTransition terminal{};
    assert(scheduler.OnTimeout(call, &terminal, effects) == voip::Error::ok);
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
    voip::SchedulerEffects effects;
    voip::CallHandle call{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:not-registered@example.test",
                                   &call, nullptr, effects) == voip::Error::agent_unavailable);
    fixture.registry.Resolve(fixture.agent)->registration =
        voip::RegistrationState::registered;
    assert(scheduler.AdmitOutgoing(fixture.agent, "", &call, nullptr, effects) ==
           voip::Error::invalid_argument);
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:one@example.test", &call,
                                   nullptr, effects) ==
           voip::Error::ok);
    voip::CallHandle queued{};
    assert(scheduler.AdmitIncoming(fixture.agent, 42, &queued, nullptr, effects) ==
           voip::Error::ok);
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
                                   effects) == voip::Error::ok);
    assert(effects.count == 1);
    assert(effects.entries[0].handle.slot == active.slot);
    effects.Clear();
    voip::CallHandle queued_a{}, queued_b{};
    assert(scheduler.AdmitIncoming(a, 10, &queued_a, nullptr, effects) ==
           voip::Error::ok);
    assert(scheduler.Answer(queued_a) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(b, "sip:b@example.test", &queued_b,
                                   nullptr, effects) ==
           voip::Error::ok);
    assert(scheduler.OnAcceptance(active) == voip::Error::ok);
    assert(scheduler.Hangup(active, nullptr, effects) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(active, nullptr, effects) ==
           voip::Error::ok);
    assert(effects.count == 2);
    assert(effects.entries[0].handle.slot == queued_a.slot);
    assert(effects.entries[0].answer_on_promotion);
    assert(!effects.entries[0].acceptance_applied);
    assert(effects.entries[1].handle.slot == queued_b.slot);
}

void test_runtime_tokens_and_lease_release_order() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::CallHandle outgoing{};
    voip::SchedulerEffects effects;
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:out@example.test",
                                   &outgoing, nullptr, effects) == voip::Error::ok);
    assert(scheduler.Resolve(outgoing)->runtime_token == 0);
    assert(scheduler.OnAcceptance(outgoing) == voip::Error::ok);
    voip::ScheduledTransition terminal{};
    assert(scheduler.Hangup(outgoing, &terminal, effects) == voip::Error::ok);
    assert(scheduler.IsLive(outgoing));
    assert(scheduler.PromotedCount() == 1);
    assert(terminal.transition.after.state == voip::CallState::terminated);
    assert(terminal.transition.terminal_event_required);
    assert(terminal.snapshot.handle.generation == outgoing.generation);
    assert(scheduler.OnTeardownComplete(outgoing, nullptr, effects) ==
           voip::Error::ok);
    assert(!scheduler.IsLive(outgoing));
    assert(scheduler.PromotedCount() == 0);

    voip::CallHandle incoming{};
    assert(scheduler.AdmitIncoming(fixture.agent, 0x1234u, &incoming,
                                   nullptr, effects) ==
           voip::Error::ok);
    assert(scheduler.Resolve(incoming)->runtime_token == 0x1234u);
    assert(scheduler.Cancel(incoming, nullptr, effects) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(incoming, nullptr, effects) ==
           voip::Error::ok);
    assert(!scheduler.IsLive(incoming));
    voip::CallHandle replacement{};
    assert(scheduler.AdmitIncoming(fixture.agent, 0x5678u, &replacement,
                                   nullptr, effects) ==
           voip::Error::ok);
    assert(replacement.slot == incoming.slot);
    assert(replacement.generation != incoming.generation);
    assert(scheduler.OnAcceptance(incoming) == voip::Error::invalid_handle);
}

void test_timeout_head_removal_promotes_next_and_clears_prefilled_effects() {
    Source source_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Sink sink_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Fixture fixture;
    voip::AgentConfig configs[2] = {fixture.config,
        {{"sip:b2@example.test", "sip:example.test", "b2", "p"},
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
    voip::CallHandle active{}, head{}, later{};
    assert(scheduler.AdmitOutgoing(a, "sip:active2@example.test", &active,
                                   nullptr, effects) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(a, "sip:head2@example.test", &head,
                                   nullptr, effects) == voip::Error::ok);
    assert(scheduler.AdmitIncoming(b, 77, &later, nullptr, effects) ==
           voip::Error::ok);
    effects.count = voip::SchedulerEffects::capacity;
    voip::ScheduledTransition terminal{};
    assert(scheduler.OnTimeout(head, &terminal, effects) == voip::Error::ok);
    assert(!scheduler.IsLive(head));
    assert(effects.count == 1);
    assert(effects.entries[0].handle.slot == later.slot);
    assert(scheduler.IsPromoted(later));
}

void test_explicit_scheduler_state_traces() {
    Fixture fixture;
    voip::CallScheduler scheduler(fixture.registry);
    voip::SchedulerEffects effects;
    voip::CallHandle call{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:trace@example.test",
                                   &call, nullptr, effects) == voip::Error::ok);
    voip::ScheduledTransition transition{};
    assert(scheduler.OnAcceptance(call, &transition) == voip::Error::ok);
    assert(transition.transition.before.state == voip::CallState::initiated);
    assert(transition.transition.after.state == voip::CallState::established);
    assert(transition.transition.cause == voip::CallTransition::acceptance);
    assert(!transition.transition.terminal_event_required);
    assert(scheduler.SetHeld(call, true, &transition) == voip::Error::ok);
    assert(transition.transition.before.hold_reason == voip::HoldReason::none);
    assert(transition.transition.after.hold_reason == voip::HoldReason::media);
    assert(transition.transition.cause == voip::CallTransition::hold);
    assert(scheduler.SetHeld(call, false, &transition) == voip::Error::ok);
    assert(transition.transition.before.hold_reason == voip::HoldReason::media);
    assert(transition.transition.after.state == voip::CallState::established);
    assert(transition.transition.cause == voip::CallTransition::resume);
    assert(scheduler.Hangup(call, &transition, effects) == voip::Error::ok);
    assert(transition.transition.after.state == voip::CallState::terminated);
    assert(transition.transition.cause == voip::CallTransition::finish);
    assert(transition.transition.terminal_event_required);
    assert(scheduler.IsLive(call));
    assert(scheduler.OnTeardownComplete(call, &transition, effects) ==
           voip::Error::ok);
    assert(transition.transition.before.state == voip::CallState::terminated);
    assert(transition.transition.after.state == voip::CallState::idle);
    assert(!transition.transition.terminal_event_required);
    assert(transition.handle_invalidated);
    assert(!scheduler.IsLive(call));

    voip::CallHandle active{}, waiting{};
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:active-trace@example.test",
                                   &active, nullptr, effects) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(fixture.agent, "sip:waiting-trace@example.test",
                                   &waiting, nullptr, effects) == voip::Error::ok);
    voip::CallSnapshot snapshot = scheduler.Snapshot(waiting);
    assert(snapshot.state == voip::CallState::hold);
    assert(snapshot.hold_reason == voip::HoldReason::waiting);
    assert(scheduler.OnAcceptance(active) == voip::Error::ok);
    assert(scheduler.Hangup(active, nullptr, effects) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(active, nullptr, effects) ==
           voip::Error::ok);
    assert(effects.count == 1);
    assert(effects.entries[0].handle.slot == waiting.slot);
    snapshot = scheduler.Snapshot(waiting);
    assert(snapshot.state == voip::CallState::hold);
    assert(snapshot.hold_reason == voip::HoldReason::waiting);
    assert(scheduler.OnAcceptance(waiting, &transition) == voip::Error::ok);
    assert(transition.transition.before.state == voip::CallState::hold);
    assert(transition.transition.before.hold_reason == voip::HoldReason::waiting);
    assert(transition.transition.after.state == voip::CallState::established);
    assert(transition.transition.cause == voip::CallTransition::acceptance);
    assert(scheduler.Hangup(waiting, &transition, effects) == voip::Error::ok);
    assert(scheduler.OnTeardownComplete(waiting, nullptr, effects) ==
           voip::Error::ok);
}

void test_full_capacity_restores_all_fixed_resources() {
    Source source_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Sink sink_b{voip::PcmFormat{8000, 160, 1, voip::SampleFormat::signed_16}};
    Fixture fixture;
    voip::AgentConfig configs[2] = {fixture.config,
        {{"sip:capacity@example.test", "sip:example.test", "capacity", "p"},
         {&source_b, &sink_b}, true}};
    voip::ServiceConfig service = fixture.service;
    service.agents = configs;
    service.agent_count = 2;
    assert(fixture.registry.Initialize(service) == voip::Error::ok);
    voip::AgentHandle agents[2]{};
    assert(fixture.registry.GetAgentHandle(0, &agents[0]) == voip::Error::ok);
    assert(fixture.registry.GetAgentHandle(1, &agents[1]) == voip::Error::ok);
    fixture.registry.Resolve(agents[0])->registration = voip::RegistrationState::registered;
    fixture.registry.Resolve(agents[1])->registration = voip::RegistrationState::registered;
    voip::CallScheduler scheduler(fixture.registry);
    voip::SchedulerEffects effects;
    voip::CallHandle calls[7]{};
    assert(scheduler.AdmitOutgoing(agents[0], "sip:cap-a@example.test", &calls[0],
                                   nullptr, effects) == voip::Error::ok);
    assert(scheduler.AdmitOutgoing(agents[1], "sip:cap-b@example.test", &calls[1],
                                   nullptr, effects) == voip::Error::ok);
    for (std::size_t i = 2; i < 7; ++i) {
        char uri[64]{};
        std::snprintf(uri, sizeof(uri), "sip:cap-%zu@example.test", i);
        assert(scheduler.AdmitOutgoing(agents[i % 2], uri, &calls[i],
                                       nullptr, effects) == voip::Error::ok);
    }
    for (const voip::CallHandle call : calls) assert(scheduler.IsLive(call));
    assert(scheduler.LiveCount() == 7);
    assert(scheduler.PromotedCount() == 2);
    assert(scheduler.QueuedCount() == 5);
    assert(scheduler.AvailableLogicalCalls() == 0);
    assert(scheduler.AvailablePromotedCalls() == 0);
    assert(scheduler.AvailableFifoEntries() == 0);

    for (std::size_t i = 0; i < 2; ++i) {
        assert(scheduler.OnAcceptance(calls[i]) == voip::Error::ok);
        voip::ScheduledTransition terminal{};
        assert(scheduler.Hangup(calls[i], &terminal, effects) == voip::Error::ok);
        assert(terminal.snapshot.handle.slot == calls[i].slot);
        assert(terminal.snapshot.handle.generation == calls[i].generation);
        assert(terminal.transition.terminal_event_required);
        assert(scheduler.IsLive(calls[i]));
        assert(scheduler.OnTeardownComplete(calls[i], nullptr, effects) ==
               voip::Error::ok);
        assert(!scheduler.IsLive(calls[i]));
        assert(scheduler.OnAcceptance(calls[i]) == voip::Error::invalid_handle);
        assert(scheduler.Cancel(calls[i], nullptr, effects) ==
               voip::Error::invalid_handle);
    }
    for (std::size_t i = 2; i < 7; ++i) {
        assert(scheduler.IsLive(calls[i]));
        voip::ScheduledTransition terminal{};
        if (scheduler.IsPromoted(calls[i])) {
            const voip::CallSnapshot snapshot = scheduler.Snapshot(calls[i]);
            if (snapshot.state == voip::CallState::initiated ||
                snapshot.state == voip::CallState::hold)
                assert(scheduler.OnAcceptance(calls[i]) == voip::Error::ok);
            assert(scheduler.Hangup(calls[i], &terminal, effects) == voip::Error::ok);
            assert(terminal.snapshot.handle.slot == calls[i].slot);
            assert(terminal.snapshot.handle.generation == calls[i].generation);
            assert(terminal.transition.terminal_event_required);
            assert(scheduler.IsLive(calls[i]));
            assert(scheduler.OnTeardownComplete(calls[i], nullptr, effects) ==
                   voip::Error::ok);
        } else {
            assert(scheduler.Cancel(calls[i], &terminal, effects) == voip::Error::ok);
            assert(terminal.snapshot.handle.slot == calls[i].slot);
            assert(terminal.snapshot.handle.generation == calls[i].generation);
            assert(terminal.transition.terminal_event_required);
            assert(terminal.handle_invalidated);
        }
        assert(!scheduler.IsLive(calls[i]));
        assert(scheduler.OnAcceptance(calls[i]) == voip::Error::invalid_handle);
        assert(scheduler.Cancel(calls[i], nullptr, effects) ==
               voip::Error::invalid_handle);
    }
    assert(scheduler.LiveCount() == 0);
    assert(scheduler.PromotedCount() == 0);
    assert(scheduler.QueuedCount() == 0);
    assert(scheduler.AvailableLogicalCalls() == 7);
    assert(scheduler.AvailablePromotedCalls() == 2);
    assert(scheduler.AvailableFifoEntries() == 5);
    for (const voip::CallHandle call : calls) assert(!scheduler.IsLive(call));
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
    test_timeout_head_removal_promotes_next_and_clears_prefilled_effects();
    test_explicit_scheduler_state_traces();
    test_full_capacity_restores_all_fixed_resources();
    std::puts("CallSchedulerTest PASSED");
    return 0;
}
