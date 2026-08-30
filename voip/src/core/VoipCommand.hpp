#ifndef VOIP_CORE_VOIP_COMMAND_HPP
#define VOIP_CORE_VOIP_COMMAND_HPP

#include <voip/VoipTypes.hpp>

#include <cstddef>
#include <cstdint>

namespace voip {

enum class CommandType : std::uint8_t {
    dial,
    answer,
    reject,
    cancel,
    hangup,
    set_held,
    shutdown,
};

struct DialCommand {
    AgentHandle agent{};
    char remote_uri[max_uri_length + 1]{};
};

struct CallCommand {
    CallHandle call{};
};

struct RejectCommand {
    CallHandle call{};
    std::uint16_t sip_status = 0;
    char reason[max_reason_length + 1]{};
};

struct SetHeldCommand {
    CallHandle call{};
    bool held = false;
};

// Every field is a value. In particular, shutdown does not carry a pointer to
// a caller-owned completion object or any other caller-stack storage.
struct VoipCommand {
    CommandType type = CommandType::shutdown;
    OperationId operation = 0;
    union {
        DialCommand dial;
        CallCommand answer;
        RejectCommand reject;
        CallCommand cancel;
        CallCommand hangup;
        SetHeldCommand set_held;
        struct {
        } shutdown;
    };
};

// Validation is a synchronous borrow: CommandMailbox never stores this
// interface or any context behind it. Implementations may bind generation-
// safe AgentRegistry and CallScheduler pools to the two methods.
class CommandHandleValidator {
public:
    virtual ~CommandHandleValidator() noexcept = default;
    virtual bool Validate(AgentHandle) const noexcept = 0;
    virtual bool Validate(CallHandle) const noexcept = 0;
};

static_assert(sizeof(VoipCommand) > 0, "command must have fixed storage");

bool ValidateCommand(const VoipCommand &command) noexcept;

} // namespace voip

#endif
