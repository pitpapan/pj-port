#include "../../src/core/CallStateMachine.hpp"

#include <cassert>
#include <cstdio>

namespace {

using voip::CallProjection;
using voip::CallState;
using voip::CallStateMachine;
using voip::CallTransition;
using voip::Error;
using voip::HoldReason;

void expect_edge(CallStateMachine &machine, CallTransition cause,
                 CallState state, HoldReason reason, bool terminal) {
    voip::AppliedCallTransition applied{};
    assert(machine.Apply(cause, &applied) == Error::ok);
    assert(applied.before.state != applied.after.state ||
           applied.before.hold_reason != applied.after.hold_reason);
    assert(applied.after.state == state);
    assert(applied.after.hold_reason == reason);
    assert(applied.terminal_event_required == terminal);
    assert(machine.Snapshot().state == state);
    assert(machine.Snapshot().hold_reason == reason);
}

void test_literal_normative_edges() {
    CallStateMachine machine;
    expect_edge(machine, CallTransition::initiation, CallState::initiated,
                HoldReason::none, false);
    expect_edge(machine, CallTransition::wait, CallState::hold,
                HoldReason::waiting, false);
    expect_edge(machine, CallTransition::acceptance, CallState::established,
                HoldReason::none, false);
    expect_edge(machine, CallTransition::hold, CallState::hold,
                HoldReason::media, false);
    expect_edge(machine, CallTransition::resume, CallState::established,
                HoldReason::none, false);
    expect_edge(machine, CallTransition::finish, CallState::terminated,
                HoldReason::none, true);
    expect_edge(machine, CallTransition::cleanup, CallState::idle,
                HoldReason::none, false);

    CallStateMachine rejected;
    expect_edge(rejected, CallTransition::initiation, CallState::initiated,
                HoldReason::none, false);
    expect_edge(rejected, CallTransition::rejection, CallState::terminated,
                HoldReason::none, true);
    expect_edge(rejected, CallTransition::cleanup, CallState::idle,
                HoldReason::none, false);

    CallStateMachine direct_timeout;
    expect_edge(direct_timeout, CallTransition::initiation,
                CallState::initiated, HoldReason::none, false);
    expect_edge(direct_timeout, CallTransition::timeout, CallState::idle,
                HoldReason::none, true);
}

void test_waiting_acceptance_and_invalid_edges_are_immutable() {
    CallStateMachine machine;
    expect_edge(machine, CallTransition::initiation, CallState::initiated,
                HoldReason::none, false);
    expect_edge(machine, CallTransition::wait, CallState::hold,
                HoldReason::waiting, false);
    expect_edge(machine, CallTransition::acceptance, CallState::established,
                HoldReason::none, false);

    CallProjection before = machine.Snapshot();
    voip::AppliedCallTransition output{};
    output.after = before;
    assert(machine.Apply(CallTransition::resume, &output) ==
           Error::invalid_state);
    assert(machine.Snapshot().state == before.state);
    assert(machine.Snapshot().hold_reason == before.hold_reason);
    assert(output.after.state == before.state);
    assert(output.after.hold_reason == before.hold_reason);

    CallStateMachine media;
    expect_edge(media, CallTransition::initiation, CallState::initiated,
                HoldReason::none, false);
    expect_edge(media, CallTransition::acceptance, CallState::established,
                HoldReason::none, false);
    expect_edge(media, CallTransition::hold, CallState::hold,
                HoldReason::media, false);
    expect_edge(media, CallTransition::resume, CallState::established,
                HoldReason::none, false);
    assert(media.Apply(CallTransition::acceptance, &output) ==
           Error::invalid_state);
}

} // namespace

int main() {
    test_literal_normative_edges();
    test_waiting_acceptance_and_invalid_edges_are_immutable();
    std::puts("CallStateMachineTest PASSED");
    return 0;
}
