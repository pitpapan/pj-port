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
    CallHandle handle{};
    AgentHandle agent{};
    CallDirection direction = CallDirection::outgoing;
    CallStateMachine state_machine{};
    std::uint32_t runtime_token = 0;
    bool answer_on_promotion = false;
    char remote_uri[max_uri_length + 1]{};

    CallContext() noexcept = default;
    ~CallContext() noexcept = default;
    CallContext(const CallContext &) = delete;
    CallContext &operator=(const CallContext &) = delete;
};

} // namespace voip

#endif
