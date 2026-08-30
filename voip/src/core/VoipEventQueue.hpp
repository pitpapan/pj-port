#ifndef VOIP_CORE_VOIP_EVENT_QUEUE_HPP
#define VOIP_CORE_VOIP_EVENT_QUEUE_HPP

#include "CoreSynchronization.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <voip/VoipEvents.hpp>

namespace voip {

// Fixed-storage, multi-producer/single-polling-consumer event queue.  Public
// event records are copied at commit and contain no PJPROJECT state.  The
// polling consumer is intentionally single-consumer; callers must serialize
// TryPop/WaitPop calls.
class VoipEventQueue final {
public:
    static constexpr std::size_t capacity = 32;
    static constexpr std::size_t ordinary_capacity = capacity - 1;

    // A token is owned by its producer while it is active and must be
    // externally serialized across a handoff to the commit/cancel owner.
    // Queue destruction and token use must not race. Distinct active tokens
    // may be committed or cancelled concurrently; queue state is locked.
    class Reservation final {
    public:
        Reservation() noexcept = default;
        ~Reservation() noexcept;

        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;

        bool Commit(const Event &event) noexcept;
        bool Cancel() noexcept;
        bool IsActive() const noexcept { return active_; }
        bool IsGuaranteed() const noexcept { return guaranteed_; }
        bool IsPermanent() const noexcept { return permanent_; }

    private:
        friend class VoipEventQueue;
        VoipEventQueue *queue_ = nullptr;
        bool active_ = false;
        bool guaranteed_ = false;
        bool permanent_ = false;
    };

    VoipEventQueue() noexcept = default;
    ~VoipEventQueue() noexcept;

    VoipEventQueue(const VoipEventQueue &) = delete;
    VoipEventQueue &operator=(const VoipEventQueue &) = delete;

    // Reserve checks capacity atomically but does not modify the supplied
    // Event. Commit or Cancel consumes the token exactly once.
    bool Reserve(const Event &event, Reservation *reservation) noexcept;
    bool ReserveGuaranteed(const Event &event,
                           Reservation *reservation) noexcept;
    bool ReserveOrdinary(const Event &event,
                         Reservation *reservation) noexcept;

    // The stopped slot is reserved from construction through destruction and
    // can be claimed exactly once. It must be committed as service_stopped;
    // after that commit every later production attempt is rejected.
    bool ReserveServiceStopped(Reservation *reservation) noexcept;
    bool CommitServiceStopped(Reservation *reservation) noexcept;

    bool Publish(const Event &event) noexcept;
    bool TryPop(Event *event) noexcept;
    bool WaitPop(Event *event, std::uint32_t timeout_ms) noexcept;
    bool TryPeek(Event *event) noexcept;

    std::size_t Size() const noexcept;
    bool IsStopped() const noexcept;
    // Starts a fresh service lifecycle after the previous stopped record was
    // consumed. No active reservations may exist when called.
    void ResetLifecycle() noexcept;

    static bool IsGuaranteedEvent(const Event &event) noexcept;
    static bool IsCoalescibleEvent(const Event &event) noexcept;

private:
    friend class Reservation;

    bool CommitReservation(Reservation *reservation,
                           const Event &event) noexcept;
    bool CancelReservation(Reservation *reservation) noexcept;
    bool MatchesCoalescingKey(const Event &left,
                              const Event &right) noexcept;
    std::uint64_t NextSequence() noexcept;
    void DetachReservations() noexcept;

    mutable CoreMutex mutex_{};
    CoreEventSignal signal_{};
    std::array<Event, capacity> records_{};
    std::array<Reservation *, capacity> reservations_{};
    std::size_t count_ = 0;
    std::size_t reserved_count_ = 0;
    std::uint64_t next_sequence_ = 1;
    bool stopped_claimed_ = false;
    bool stopped_committed_ = false;
    Reservation *stopped_reservation_ = nullptr;
};

} // namespace voip

#endif
