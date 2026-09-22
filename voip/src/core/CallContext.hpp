#ifndef VOIP_CORE_CALL_CONTEXT_HPP
#define VOIP_CORE_CALL_CONTEXT_HPP

#include "CallStateMachine.hpp"

#include <voip/VoipTypes.hpp>

#include <cstddef>
#include <cstdint>

namespace voip {

enum class CallDirection : std::uint8_t { outgoing, incoming };

class CallScheduler;

struct CallContext {
private:
    // This phase is deliberately private to the scheduler. It is not a
    // second business-state store: CallStateMachine owns the projection.
    enum class LogicalCallPhase : std::uint8_t {
        free,
        queued_outgoing,
        queued_incoming,
        promoting,
        outgoing,
        incoming,
        early,
        established,
        held,
        disconnecting,
        disconnected,
        failed,
        cancelled,
        timed_out,
    };

    LogicalCallPhase phase = LogicalCallPhase::free;
    friend class CallScheduler;

public:
    enum class PendingOperation : std::uint8_t {
        none,
        answer,
        reject,
        cancel,
        hangup,
        set_held,
    };

    CallHandle handle{};
    AgentHandle agent{};
    CallDirection direction = CallDirection::outgoing;
    CallStateMachine state_machine{};
    std::uint32_t runtime_token = 0;
    std::uint64_t queue_started_at_ms = 0;
    std::uint64_t answer_started_at_ms = 0;
    bool queue_timer_active = false;
    bool answer_timer_active = false;
    OperationId operation = 0;
    OperationId signaling_operation = 0;
    PendingOperation pending_operation = PendingOperation::none;
    bool teardown_pending = false;
    bool native_terminal_pending = false;
    bool answer_on_promotion = false;
    char remote_uri[max_uri_length + 1]{};
    char remote_address[max_address_length + 1]{};
    std::uint16_t sip_status = 0;

    CallContext() noexcept = default;
    ~CallContext() noexcept = default;
    CallContext(const CallContext &) = delete;
    CallContext &operator=(const CallContext &) = delete;
};

} // namespace voip

#endif
