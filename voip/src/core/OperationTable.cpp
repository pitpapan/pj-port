#include "OperationTable.hpp"

#include "VoipCommand.hpp"

#include <cstring>

namespace voip {

namespace {

std::size_t BoundedLength(const char *source, std::size_t maximum) noexcept {
    if (source == nullptr) return maximum + 1;
    for (std::size_t index = 0; index <= maximum; ++index) {
        if (source[index] == '\0') return index;
    }
    return maximum + 1;
}

} // namespace

bool ValidateCommand(const VoipCommand &command) noexcept {
    switch (command.type) {
    case CommandType::dial: {
        const std::size_t length =
            BoundedLength(command.dial.remote_uri, max_uri_length);
        return command.dial.agent.IsValid() && length > 0 &&
               length <= max_uri_length;
    }
    case CommandType::answer:
        return command.answer.call.IsValid();
    case CommandType::reject:
        return command.reject.call.IsValid() &&
               BoundedLength(command.reject.reason, max_reason_length) <=
                   max_reason_length;
    case CommandType::cancel:
        return command.cancel.call.IsValid();
    case CommandType::hangup:
        return command.hangup.call.IsValid();
    case CommandType::set_held:
        return command.set_held.call.IsValid();
    case CommandType::shutdown:
        return true;
    }
    return false;
}

OperationTable::~OperationTable() noexcept {
    CoreLockGuard lock(mutex_);
    for (Record &record : records_) {
        if (record.state_ != Record::State::free) {
            (void)record.terminal_.Cancel();
            Reset(record);
        }
    }
}

OperationId OperationTable::NextCandidate(OperationId id) const noexcept {
    ++id;
    return id == 0 ? 1 : id;
}

bool OperationTable::IsLive(OperationId id) const noexcept {
    if (id == 0) return false;
    for (const Record &record : records_)
        if (record.state_ != Record::State::free && record.id_ == id)
            return true;
    return false;
}

bool OperationTable::Reserve(VoipEventQueue &events, OperationId *id) noexcept {
    if (id != nullptr) *id = 0;
    if (id == nullptr) return false;

    CoreLockGuard lock(mutex_);
    Record *free_record = nullptr;
    for (Record &candidate : records_) {
        if (candidate.state_ == Record::State::free) {
            free_record = &candidate;
            break;
        }
    }
    if (free_record == nullptr) return false;

    OperationId candidate = next_id_ == 0 ? 1 : next_id_;
    for (std::size_t attempts = 0; attempts <= capacity; ++attempts) {
        if (!IsLive(candidate)) break;
        candidate = NextCandidate(candidate);
    }
    if (IsLive(candidate)) return false;

    Event terminal{};
    terminal.type = EventType::operation_terminal;
    terminal.operation = candidate;
    if (!events.ReserveGuaranteed(terminal, &free_record->terminal_))
        return false;

    free_record->id_ = candidate;
    free_record->state_ = Record::State::provisional;
    next_id_ = NextCandidate(candidate);
    *id = candidate;
    return true;
}

bool OperationTable::CopyReason(char (&destination)[max_reason_length + 1],
                                const char *source) noexcept {
    const std::size_t length = BoundedLength(source, max_reason_length);
    if (length > max_reason_length) {
        if (source == nullptr) {
            destination[0] = '\0';
            return true;
        }
        std::memcpy(destination, source, max_reason_length);
        destination[max_reason_length] = '\0';
        return true;
    }
    if (source == nullptr) {
        destination[0] = '\0';
        return true;
    }
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

void OperationTable::Reset(Record &record) noexcept {
    record.id_ = 0;
    record.state_ = Record::State::free;
}

bool OperationTable::CompleteRecord(Record &record, Error error,
                                    std::uint16_t sip_status,
                                    const char *reason) noexcept {
    if (record.state_ != Record::State::accepted) return false;
    Event terminal{};
    terminal.type = EventType::operation_terminal;
    terminal.operation = record.id_;
    terminal.status.error = error;
    terminal.status.sip_status = sip_status;
    if (!CopyReason(terminal.status.reason, reason)) return false;

    // A failed commit is an observable invariant violation. Keep both record
    // and reservation active so a runtime can retry before stopping events.
    if (!record.terminal_.Commit(terminal)) return false;
    Reset(record);
    return true;
}

bool OperationTable::Complete(OperationId id, Error error,
                              std::uint16_t sip_status,
                              const char *reason) noexcept {
    if (id == 0) return false;
    CoreLockGuard lock(mutex_);
    for (Record &record : records_) {
        if (record.state_ == Record::State::accepted && record.id_ == id)
            return CompleteRecord(record, error, sip_status, reason);
    }
    return false;
}

bool OperationTable::AcceptAdmission(OperationId id) noexcept {
    if (id == 0) return false;
    CoreLockGuard lock(mutex_);
    for (Record &record : records_) {
        if (record.state_ == Record::State::provisional && record.id_ == id) {
            record.state_ = Record::State::accepted;
            return true;
        }
    }
    return false;
}

bool OperationTable::RollbackAdmission(OperationId id) noexcept {
    if (id == 0) return false;
    CoreLockGuard lock(mutex_);
    for (Record &record : records_) {
        if (record.state_ != Record::State::provisional || record.id_ != id)
            continue;
        const bool cancelled = record.terminal_.Cancel();
        // Cancellation is used by admission rollback. If no later reserve
        // advanced the cursor, put this ID back so failed admissions never
        // consume operation IDs.
        if (next_id_ == NextCandidate(record.id_)) next_id_ = record.id_;
        Reset(record);
        return cancelled;
    }
    return false;
}

std::size_t OperationTable::ActiveCount() const noexcept {
    CoreLockGuard lock(mutex_);
    std::size_t count = 0;
    for (const Record &record : records_)
        if (record.state_ != Record::State::free) ++count;
    return count;
}

std::size_t OperationTable::Available() const noexcept {
    return capacity - ActiveCount();
}

} // namespace voip
