#include "CommandMailbox.hpp"

namespace voip {

Error CommandMailbox::AdmitDial(OperationTable &operations,
                                VoipEventQueue &events, AgentHandle agent,
                                const DialRequest &request,
                                OperationId *operation,
                                const CommandHandleValidator *validator) noexcept {
    if (operation != nullptr) *operation = 0;
    if (request.remote_uri == nullptr) return Error::invalid_argument;
    VoipCommand command{};
    command.type = CommandType::dial;
    command.dial.agent = agent;
    std::size_t length = 0;
    while (length <= max_uri_length && request.remote_uri[length] != '\0')
        ++length;
    if (length == 0 || length > max_uri_length)
        return Error::invalid_argument;
    for (std::size_t index = 0; index <= length; ++index)
        command.dial.remote_uri[index] = request.remote_uri[index];
    return Admit(operations, events, command, operation, validator);
}

Error CommandMailbox::Admit(OperationTable &operations, VoipEventQueue &events,
                            const VoipCommand &command, OperationId *operation,
                            const CommandHandleValidator *validator) noexcept {
    if (operation != nullptr) *operation = 0;
    CoreLockGuard lock(mutex_);
    if (!ValidateCommand(command)) return Error::invalid_argument;
    if (command.type == CommandType::shutdown)
        return Error::invalid_argument;
    if (command.type != CommandType::shutdown) {
        if (validator == nullptr) return Error::invalid_handle;
        switch (command.type) {
        case CommandType::dial:
            if (!validator->Validate(command.dial.agent))
                return Error::invalid_handle;
            break;
        case CommandType::answer:
            if (!validator->Validate(command.answer.call))
                return Error::invalid_handle;
            break;
        case CommandType::reject:
            if (!validator->Validate(command.reject.call))
                return Error::invalid_handle;
            break;
        case CommandType::cancel:
            if (!validator->Validate(command.cancel.call))
                return Error::invalid_handle;
            break;
        case CommandType::hangup:
            if (!validator->Validate(command.hangup.call))
                return Error::invalid_handle;
            break;
        case CommandType::set_held:
            if (!validator->Validate(command.set_held.call))
                return Error::invalid_handle;
            break;
        case CommandType::shutdown:
            break;
        }
    }

    OperationId id = 0;
    if (!operations.Reserve(events, &id)) {
        if (operations.ActiveCount() == OperationTable::capacity)
            return Error::resource_exhausted;
        return Error::queue_full;
    }
    if (count_ == capacity) {
        (void)operations.RollbackAdmission(id);
        return Error::queue_full;
    }

    VoipCommand copied = command;
    copied.operation = id;
    if (!operations.AcceptAdmission(id)) {
        (void)operations.RollbackAdmission(id);
        return Error::internal_failure;
    }
    records_[write_] = copied;
    write_ = (write_ + 1) % capacity;
    ++count_;
    if (operation != nullptr) *operation = id;
    return Error::ok;
}

bool CommandMailbox::TryPushShutdown() noexcept {
    CoreLockGuard lock(mutex_);
    if (count_ == capacity || shutdown_enqueued_) return false;
    VoipCommand command{};
    command.type = CommandType::shutdown;
    command.operation = 0;
    records_[write_] = command;
    write_ = (write_ + 1) % capacity;
    ++count_;
    shutdown_enqueued_ = true;
    return true;
}

bool CommandMailbox::TryPop(VoipCommand *command) noexcept {
    if (command == nullptr) return false;
    CoreLockGuard lock(mutex_);
    if (count_ == 0) return false;
    *command = records_[read_];
    read_ = (read_ + 1) % capacity;
    --count_;
    return true;
}

std::size_t CommandMailbox::Size() const noexcept {
    CoreLockGuard lock(mutex_);
    return count_;
}

std::size_t CommandMailbox::Available() const noexcept {
    return capacity - Size();
}

} // namespace voip
