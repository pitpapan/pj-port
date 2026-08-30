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

void test_every_state_reason_and_cause_is_exactly_normative() {
    const CallState states[] = {CallState::idle, CallState::initiated,
                                CallState::established, CallState::hold,
                                CallState::terminated};
    const HoldReason reasons[] = {HoldReason::none, HoldReason::waiting,
                                  HoldReason::media};
    const CallTransition causes[] = {
        CallTransition::initiation, CallTransition::acceptance,
        CallTransition::rejection, CallTransition::wait,
        CallTransition::timeout, CallTransition::hold,
        CallTransition::resume, CallTransition::finish,
        CallTransition::cleanup};
    for (CallState state : states) {
        for (HoldReason reason : reasons) {
            const bool reachable =
                (state == CallState::idle && reason == HoldReason::none) ||
                (state == CallState::initiated && reason == HoldReason::none) ||
                (state == CallState::established && reason == HoldReason::none) ||
                (state == CallState::hold &&
                 (reason == HoldReason::waiting || reason == HoldReason::media)) ||
                (state == CallState::terminated && reason == HoldReason::none);
            if (!reachable) continue;
            for (CallTransition cause : causes) {
                CallStateMachine machine;
                if (state != CallState::idle) {
                    voip::AppliedCallTransition applied{};
                    assert(machine.Apply(CallTransition::initiation, &applied) ==
                           Error::ok);
                    if (state == CallState::established) {
                        assert(machine.Apply(CallTransition::acceptance,
                                              &applied) == Error::ok);
                    } else if (state == CallState::hold) {
                        assert(machine.Apply(CallTransition::wait, &applied) ==
                               Error::ok);
                        if (reason == HoldReason::media) {
                            assert(machine.Apply(CallTransition::acceptance,
                                                  &applied) == Error::ok);
                            assert(machine.Apply(CallTransition::hold, &applied) ==
                                   Error::ok);
                        }
                    } else if (state == CallState::terminated) {
                        assert(machine.Apply(CallTransition::rejection,
                                              &applied) == Error::ok);
                    }
                }
                const CallProjection before = machine.Snapshot();
                voip::AppliedCallTransition output{};
                output.after = before;
                const Error actual = machine.Apply(cause, &output);
                bool valid = false;
                CallProjection expected{};
                bool terminal = false;
                if (state == CallState::idle && reason == HoldReason::none &&
                    cause == CallTransition::initiation) {
                    valid = true;
                    expected = {CallState::initiated, HoldReason::none};
                } else if (state == CallState::initiated &&
                           reason == HoldReason::none) {
                    if (cause == CallTransition::acceptance) {
                        valid = true;
                        expected = {CallState::established, HoldReason::none};
                    } else if (cause == CallTransition::rejection) {
                        valid = true;
                        expected = {CallState::terminated, HoldReason::none};
                        terminal = true;
                    } else if (cause == CallTransition::wait) {
                        valid = true;
                        expected = {CallState::hold, HoldReason::waiting};
                    } else if (cause == CallTransition::timeout) {
                        valid = true;
                        expected = {CallState::idle, HoldReason::none};
                        terminal = true;
                    }
                } else if (state == CallState::established &&
                           reason == HoldReason::none) {
                    if (cause == CallTransition::hold) {
                        valid = true;
                        expected = {CallState::hold, HoldReason::media};
                    } else if (cause == CallTransition::finish) {
                        valid = true;
                        expected = {CallState::terminated, HoldReason::none};
                        terminal = true;
                    }
                } else if (state == CallState::hold &&
                           reason == HoldReason::waiting &&
                           cause == CallTransition::acceptance) {
                    valid = true;
                    expected = {CallState::established, HoldReason::none};
                } else if (state == CallState::hold &&
                           reason == HoldReason::media &&
                           cause == CallTransition::resume) {
                    valid = true;
                    expected = {CallState::established, HoldReason::none};
                } else if (state == CallState::hold &&
                           (reason == HoldReason::waiting ||
                            reason == HoldReason::media) &&
                           (cause == CallTransition::finish ||
                            cause == CallTransition::rejection ||
                            cause == CallTransition::timeout)) {
                    valid = true;
                    expected = {CallState::terminated, HoldReason::none};
                    terminal = true;
                } else if (state == CallState::terminated &&
                           reason == HoldReason::none &&
                           cause == CallTransition::cleanup) {
                    valid = true;
                    expected = {CallState::idle, HoldReason::none};
                }
                if (valid) {
                    assert(actual == Error::ok);
                    assert(output.after.state == expected.state);
                    assert(output.after.hold_reason == expected.hold_reason);
                    assert(output.terminal_event_required == terminal);
                } else {
                    assert(actual == Error::invalid_state);
                    assert(machine.Snapshot().state == before.state);
                    assert(machine.Snapshot().hold_reason == before.hold_reason);
                    assert(output.after.state == before.state);
                    assert(output.after.hold_reason == before.hold_reason);
                }
            }
        }
    }
    CallStateMachine machine;
    assert(machine.Apply(CallTransition::initiation, nullptr) ==
           Error::invalid_argument);
}

} // namespace

int main() {
    test_literal_normative_edges();
    test_waiting_acceptance_and_invalid_edges_are_immutable();
    test_every_state_reason_and_cause_is_exactly_normative();
    std::puts("CallStateMachineTest PASSED");
    return 0;
}
