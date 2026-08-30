#ifndef VOIP_CORE_CALL_STATE_MACHINE_HPP
#define VOIP_CORE_CALL_STATE_MACHINE_HPP

#include <voip/VoipTypes.hpp>

namespace voip {

struct CallProjection {
    CallState state = CallState::idle;
    HoldReason hold_reason = HoldReason::none;
};

struct AppliedCallTransition {
    CallProjection before{};
    CallProjection after{};
    CallTransition cause = CallTransition::initiation;
    bool terminal_event_required = false;
};

class CallStateMachine final {
public:
    CallStateMachine() noexcept = default;
    CallProjection Snapshot() const noexcept { return projection_; }

    Error Apply(CallTransition cause,
                AppliedCallTransition *applied) noexcept;

private:
    CallProjection projection_{};
};

} // namespace voip

#endif
