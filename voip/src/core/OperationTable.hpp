#ifndef VOIP_CORE_OPERATION_TABLE_HPP
#define VOIP_CORE_OPERATION_TABLE_HPP

#include "VoipEventQueue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voip {

class OperationTable final {
public:
    static constexpr std::size_t capacity = 16;

    explicit OperationTable(OperationId next_id = 1) noexcept
        : next_id_(next_id == 0 ? 1 : next_id) {}
    ~OperationTable() noexcept;

    OperationTable(const OperationTable &) = delete;
    OperationTable &operator=(const OperationTable &) = delete;

    bool Reserve(VoipEventQueue &events, OperationId *id) noexcept;
    bool AcceptAdmission(OperationId id) noexcept;
    bool Complete(OperationId id, Error error, std::uint16_t sip_status = 0,
                  const char *reason = nullptr) noexcept;
    // Only a provisional admission may be rolled back. Accepted operations
    // retain their terminal reservation until Complete succeeds.
    bool RollbackAdmission(OperationId id) noexcept;

    std::size_t ActiveCount() const noexcept;
    std::size_t Available() const noexcept;
private:
    static bool CopyReason(char (&destination)[max_reason_length + 1],
                           const char *source) noexcept;
    struct Record {
        enum class State : std::uint8_t { free, provisional, accepted };
        OperationId id_ = 0;
        State state_ = State::free;
        VoipEventQueue::Reservation terminal_{};
    };
    static void Reset(Record &record) noexcept;
    bool IsLive(OperationId id) const noexcept;
    OperationId NextCandidate(OperationId id) const noexcept;
    bool CompleteRecord(Record &record, Error error,
                       std::uint16_t sip_status,
                       const char *reason) noexcept;

    mutable CoreMutex mutex_{};
    std::array<Record, capacity> records_{};
    OperationId next_id_ = 1;
};

static_assert(OperationTable::capacity == 16,
              "operation capacity is a fixed product limit");

} // namespace voip

#endif
