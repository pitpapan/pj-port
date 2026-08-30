#include "VoipEventQueue.hpp"

#include <chrono>

namespace voip {

VoipEventQueue::Reservation::~Reservation() noexcept {
    if (active_) (void)Cancel();
}

bool VoipEventQueue::Reservation::Commit(const Event &event) noexcept {
    return active_ && queue_ != nullptr &&
           queue_->CommitReservation(this, event);
}

bool VoipEventQueue::Reservation::Cancel() noexcept {
    return active_ && queue_ != nullptr && queue_->CancelReservation(this);
}

VoipEventQueue::~VoipEventQueue() noexcept { DetachReservations(); }

bool VoipEventQueue::IsGuaranteedEvent(const Event &event) noexcept {
    switch (event.type) {
    case EventType::incoming_call:
    case EventType::operation_terminal:
    case EventType::service_stopped:
        return true;
    case EventType::call_state:
        return event.destination_state == CallState::terminated ||
               event.call_snapshot.state == CallState::terminated;
    case EventType::agent_snapshot:
        return event.agent_snapshot.registration ==
                   RegistrationState::authentication_failed ||
               event.agent_snapshot.registration ==
                   RegistrationState::transport_failed;
    case EventType::media_snapshot:
    case EventType::resource_snapshot:
        return false;
    }
    return false;
}

bool VoipEventQueue::IsCoalescibleEvent(const Event &event) noexcept {
    if (IsGuaranteedEvent(event)) return false;
    return event.type == EventType::agent_snapshot ||
           event.type == EventType::call_state ||
           event.type == EventType::media_snapshot ||
           event.type == EventType::resource_snapshot;
}

bool VoipEventQueue::Reserve(const Event &event,
                             Reservation *reservation) noexcept {
    if (reservation == nullptr || reservation->active_ ||
        event.type == EventType::service_stopped)
        return false;
    CoreLockGuard lock(mutex_);
    if (stopped_committed_ || reserved_count_ >= capacity) return false;
    if (count_ + reserved_count_ >= ordinary_capacity) {
        bool has_match = false;
        if (IsCoalescibleEvent(event)) {
            for (std::size_t index = 0; index < count_; ++index) {
                if (MatchesCoalescingKey(records_[index], event)) {
                    has_match = true;
                    break;
                }
            }
        }
        if (!has_match) return false;
    }
    reservation->queue_ = this;
    reservation->active_ = true;
    reservation->guaranteed_ = IsGuaranteedEvent(event);
    reservation->permanent_ = false;
    reservations_[reserved_count_] = reservation;
    ++reserved_count_;
    return true;
}

bool VoipEventQueue::ReserveGuaranteed(const Event &event,
                                       Reservation *reservation) noexcept {
    return IsGuaranteedEvent(event) && Reserve(event, reservation);
}

bool VoipEventQueue::ReserveOrdinary(const Event &event,
                                     Reservation *reservation) noexcept {
    return !IsGuaranteedEvent(event) && Reserve(event, reservation);
}

bool VoipEventQueue::ReserveServiceStopped(Reservation *reservation) noexcept {
    if (reservation == nullptr || reservation->active_) return false;
    CoreLockGuard lock(mutex_);
    if (stopped_claimed_ || stopped_committed_) return false;
    stopped_claimed_ = true;
    reservation->queue_ = this;
    reservation->active_ = true;
    reservation->guaranteed_ = true;
    reservation->permanent_ = true;
    stopped_reservation_ = reservation;
    return true;
}

bool VoipEventQueue::CommitServiceStopped(Reservation *reservation) noexcept {
    if (reservation == nullptr || !reservation->active_ ||
        !reservation->permanent_)
        return false;
    Event stopped{};
    stopped.type = EventType::service_stopped;
    return reservation->Commit(stopped);
}

bool VoipEventQueue::Publish(const Event &event) noexcept {
    if (event.type == EventType::service_stopped) return false;
    Reservation reservation;
    if (!Reserve(event, &reservation)) return false;
    return reservation.Commit(event);
}

std::uint64_t VoipEventQueue::NextSequence() noexcept {
    const std::uint64_t sequence = next_sequence_;
    ++next_sequence_;
    if (next_sequence_ == 0) next_sequence_ = 1;
    return sequence == 0 ? NextSequence() : sequence;
}

bool VoipEventQueue::MatchesCoalescingKey(const Event &left,
                                          const Event &right) noexcept {
    if (!IsCoalescibleEvent(left) || !IsCoalescibleEvent(right) ||
        left.type != right.type)
        return false;
    switch (left.type) {
    case EventType::agent_snapshot: {
        const AgentHandle left_handle =
            left.agent_snapshot.handle.IsValid() ? left.agent_snapshot.handle
                                                  : left.agent;
        const AgentHandle right_handle =
            right.agent_snapshot.handle.IsValid() ? right.agent_snapshot.handle
                                                   : right.agent;
        return left_handle.slot == right_handle.slot &&
               left_handle.generation == right_handle.generation;
    }
    case EventType::call_state:
    case EventType::media_snapshot: {
        const CallHandle left_handle =
            left.call_snapshot.handle.IsValid() ? left.call_snapshot.handle
                                                : left.call;
        const CallHandle right_handle =
            right.call_snapshot.handle.IsValid() ? right.call_snapshot.handle
                                                 : right.call;
        return left_handle.slot == right_handle.slot &&
               left_handle.generation == right_handle.generation;
    }
    case EventType::resource_snapshot:
        return true;
    default:
        return false;
    }
}

bool VoipEventQueue::CommitReservation(Reservation *reservation,
                                        const Event &event) noexcept {
    if (reservation == nullptr || reservation->queue_ != this ||
        !reservation->active_ ||
        (reservation->permanent_ && event.type != EventType::service_stopped) ||
        (!reservation->permanent_ && event.type == EventType::service_stopped) ||
        (!reservation->permanent_ &&
         IsGuaranteedEvent(event) != reservation->guaranteed_))
        return false;
    bool committed = false;
    {
        CoreLockGuard lock(mutex_);
        if (!reservation->active_ ||
            (!reservation->permanent_ && stopped_committed_))
            return false;

        Event copied = event;
        copied.sequence = NextSequence();
        if (reservation->permanent_) {
            if (stopped_committed_ || count_ >= capacity) return false;
            records_[count_++] = copied;
            stopped_committed_ = true;
            committed = true;
        } else {
            for (std::size_t index = 0; index < count_; ++index) {
                if (MatchesCoalescingKey(records_[index], copied)) {
                    records_[index] = copied;
                    committed = true;
                    break;
                }
            }
            if (!committed && count_ < ordinary_capacity) {
                records_[count_++] = copied;
                committed = true;
            }
            if (!committed) return false;
        }

        for (std::size_t slot = 0; slot < reserved_count_; ++slot) {
            if (reservations_[slot] == reservation) {
                reservations_[slot] = reservations_[reserved_count_ - 1];
                reservations_[reserved_count_ - 1] = nullptr;
                --reserved_count_;
                break;
            }
        }
        reservation->active_ = false;
        reservation->queue_ = nullptr;
        reservation->guaranteed_ = false;
        reservation->permanent_ = false;
        if (stopped_reservation_ == reservation) stopped_reservation_ = nullptr;
    }
    signal_.Notify();
    return committed;
}

bool VoipEventQueue::CancelReservation(Reservation *reservation) noexcept {
    if (reservation == nullptr || reservation->queue_ != this ||
        !reservation->active_)
        return false;
    CoreLockGuard lock(mutex_);
    if (reservation->permanent_) {
        if (stopped_reservation_ != reservation || stopped_committed_)
            return false;
        // The dedicated slot remains unavailable to ordinary events, while a
        // new lifecycle owner may reclaim the uncommitted stop reservation.
        stopped_reservation_ = nullptr;
        stopped_claimed_ = false;
        reservation->active_ = false;
        reservation->queue_ = nullptr;
        reservation->guaranteed_ = false;
        reservation->permanent_ = false;
        return true;
    }
    for (std::size_t slot = 0; slot < reserved_count_; ++slot) {
        if (reservations_[slot] == reservation) {
            reservations_[slot] = reservations_[reserved_count_ - 1];
            reservations_[reserved_count_ - 1] = nullptr;
            --reserved_count_;
            reservation->active_ = false;
            reservation->queue_ = nullptr;
            reservation->guaranteed_ = false;
            return true;
        }
    }
    return false;
}

bool VoipEventQueue::TryPop(Event *event) noexcept {
    if (event == nullptr) return false;
    CoreLockGuard lock(mutex_);
    if (count_ == 0) {
        signal_.Clear();
        return false;
    }
    *event = records_[0];
    for (std::size_t index = 1; index < count_; ++index)
        records_[index - 1] = records_[index];
    --count_;
    if (count_ == 0) signal_.Clear();
    return true;
}

bool VoipEventQueue::WaitPop(Event *event, std::uint32_t timeout_ms) noexcept {
    if (event == nullptr) return false;
    if (TryPop(event)) return true;
    if (timeout_ms == 0) return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return TryPop(event);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        (void)signal_.Wait(static_cast<std::uint32_t>(remaining.count()));
        if (TryPop(event)) return true;
    }
}

bool VoipEventQueue::TryPeek(Event *event) noexcept {
    if (event == nullptr) return false;
    CoreLockGuard lock(mutex_);
    if (count_ == 0) return false;
    *event = records_[0];
    return true;
}

std::size_t VoipEventQueue::Size() const noexcept {
    CoreLockGuard lock(mutex_);
    return count_;
}

bool VoipEventQueue::IsStopped() const noexcept {
    CoreLockGuard lock(mutex_);
    return stopped_committed_;
}

void VoipEventQueue::ResetLifecycle() noexcept {
    CoreLockGuard lock(mutex_);
    count_ = 0;
    reserved_count_ = 0;
    stopped_claimed_ = false;
    stopped_committed_ = false;
    stopped_reservation_ = nullptr;
    signal_.Clear();
}

void VoipEventQueue::DetachReservations() noexcept {
    CoreLockGuard lock(mutex_);
    for (Reservation *reservation : reservations_) {
        if (reservation != nullptr && reservation->queue_ == this) {
            reservation->queue_ = nullptr;
            reservation->active_ = false;
            reservation->guaranteed_ = false;
            reservation->permanent_ = false;
        }
    }
    if (stopped_reservation_ != nullptr &&
        stopped_reservation_->queue_ == this) {
        stopped_reservation_->queue_ = nullptr;
        stopped_reservation_->active_ = false;
        stopped_reservation_->guaranteed_ = false;
        stopped_reservation_->permanent_ = false;
    }
    stopped_reservation_ = nullptr;
    reserved_count_ = 0;
}

} // namespace voip
