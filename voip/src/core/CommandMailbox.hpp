#ifndef VOIP_CORE_COMMAND_MAILBOX_HPP
#define VOIP_CORE_COMMAND_MAILBOX_HPP

#include "OperationTable.hpp"
#include "VoipCommand.hpp"

#include <array>
#include <cstddef>

namespace voip {

class CommandMailbox final {
public:
    static constexpr std::size_t capacity = 16;

    CommandMailbox() noexcept = default;
    ~CommandMailbox() noexcept = default;
    CommandMailbox(const CommandMailbox &) = delete;
    CommandMailbox &operator=(const CommandMailbox &) = delete;

    Error Admit(OperationTable &operations, VoipEventQueue &events,
                const VoipCommand &command, OperationId *operation = nullptr,
                const CommandHandleValidator *validator = nullptr) noexcept;
    Error AdmitDial(OperationTable &operations, VoipEventQueue &events,
                    AgentHandle agent, const DialRequest &request,
                    OperationId *operation = nullptr,
                    const CommandHandleValidator *validator = nullptr) noexcept;
    // The only direct producer path is service-owned shutdown completion.
    // It cannot carry an operation or caller-owned synchronization object.
    bool TryPushShutdown() noexcept;
    bool TryPop(VoipCommand *command) noexcept;
    std::size_t Size() const noexcept;
    std::size_t Available() const noexcept;

private:
    mutable CoreMutex mutex_{};
    std::array<VoipCommand, capacity> records_{};
    std::size_t read_ = 0;
    std::size_t write_ = 0;
    std::size_t count_ = 0;
    bool shutdown_enqueued_ = false;
};

static_assert(CommandMailbox::capacity == 16,
              "command capacity is a fixed product limit");

} // namespace voip

#endif
