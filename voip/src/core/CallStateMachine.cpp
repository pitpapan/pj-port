#include "CallStateMachine.hpp"

namespace voip {

Error CallStateMachine::Apply(CallTransition cause,
                              AppliedCallTransition *applied) noexcept {
    if (applied == nullptr) return Error::invalid_argument;

    CallProjection next = projection_;
    bool terminal = false;
    bool valid = true;
    switch (projection_.state) {
    case CallState::idle:
        valid = cause == CallTransition::initiation;
        if (valid) next = {CallState::initiated, HoldReason::none};
        break;
    case CallState::initiated:
        switch (cause) {
        case CallTransition::acceptance:
            next = {CallState::established, HoldReason::none};
            break;
        case CallTransition::rejection:
            next = {CallState::terminated, HoldReason::none};
            terminal = true;
            break;
        case CallTransition::wait:
            next = {CallState::hold, HoldReason::waiting};
            break;
        case CallTransition::timeout:
            next = {CallState::idle, HoldReason::none};
            terminal = true;
            break;
        default:
            valid = false;
            break;
        }
        break;
    case CallState::established:
        switch (cause) {
        case CallTransition::hold:
            next = {CallState::hold, HoldReason::media};
            break;
        case CallTransition::finish:
        case CallTransition::rejection:
        case CallTransition::timeout:
            next = {CallState::terminated, HoldReason::none};
            terminal = true;
            break;
        default:
            valid = false;
            break;
        }
        break;
    case CallState::hold:
        if (projection_.hold_reason == HoldReason::waiting &&
            cause == CallTransition::acceptance) {
            next = {CallState::established, HoldReason::none};
        } else if (projection_.hold_reason == HoldReason::media &&
                   cause == CallTransition::resume) {
            next = {CallState::established, HoldReason::none};
        } else if (cause == CallTransition::finish ||
                   cause == CallTransition::rejection ||
                   cause == CallTransition::timeout) {
            next = {CallState::terminated, HoldReason::none};
            terminal = true;
        } else {
            valid = false;
        }
        break;
    case CallState::terminated:
        if (cause == CallTransition::cleanup) {
            next = {CallState::idle, HoldReason::none};
        } else {
            valid = false;
        }
        break;
    default:
        valid = false;
        break;
    }

    if (!valid) return Error::invalid_state;
    AppliedCallTransition result{};
    result.before = projection_;
    result.after = next;
    result.cause = cause;
    result.terminal_event_required = terminal;
    projection_ = next;
    *applied = result;
    return Error::ok;
}

} // namespace voip
