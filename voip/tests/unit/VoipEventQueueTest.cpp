#include "../../src/core/VoipEventQueue.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

voip::Event Event(voip::EventType type, std::uint8_t slot = 0,
                  std::uint16_t generation = 1) {
    voip::Event event{};
    event.type = type;
    event.agent = {slot, generation};
    event.call = {slot, generation};
    event.source_state = voip::CallState::initiated;
    event.destination_state = voip::CallState::initiated;
    event.agent_snapshot.handle = event.agent;
    event.call_snapshot.handle = event.call;
    event.call_snapshot.agent = event.agent;
    return event;
}

void test_fifo_and_try_pop() {
    voip::VoipEventQueue queue;
    voip::Event output{};
    assert(!queue.TryPop(&output));
    for (std::uint8_t index = 0; index < 3; ++index) {
        voip::Event event = Event(voip::EventType::operation_terminal, index);
        event.operation = static_cast<voip::OperationId>(index + 1);
        assert(queue.Publish(event));
    }
    for (std::uint8_t index = 0; index < 3; ++index) {
        assert(queue.TryPop(&output));
        assert(output.operation == static_cast<voip::OperationId>(index + 1));
    }
    assert(!queue.TryPop(&output));
}

void test_wait_pop_timeout_and_wakeup() {
    for (unsigned iteration = 0; iteration < 100; ++iteration) {
        voip::VoipEventQueue queue;
        voip::Event output{};
        const auto start = std::chrono::steady_clock::now();
        assert(!queue.WaitPop(&output, 1));
        assert(std::chrono::steady_clock::now() >= start);
        std::thread producer([&queue, iteration]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            voip::Event event = Event(voip::EventType::resource_snapshot);
            event.resource_snapshot.active_agents =
                static_cast<std::uint8_t>(iteration);
            assert(queue.Publish(event));
        });
        assert(queue.WaitPop(&output, 100));
        assert(output.resource_snapshot.active_agents == iteration);
        producer.join();
    }
}

void test_reservation_commit_cancel_exactly_once() {
    voip::VoipEventQueue queue;
    voip::VoipEventQueue::Reservation reservation;
    voip::Event event = Event(voip::EventType::operation_terminal);
    assert(queue.Reserve(event, &reservation));
    assert(reservation.IsActive());
    assert(reservation.Commit(event));
    assert(!reservation.IsActive());
    assert(!reservation.Commit(event));
    assert(!reservation.Cancel());

    voip::VoipEventQueue::Reservation cancelled;
    assert(queue.Reserve(event, &cancelled));
    assert(cancelled.Cancel());
    assert(!cancelled.Cancel());
    assert(!cancelled.Commit(event));
    assert(queue.Size() == 1);
}

void test_guaranteed_classification_and_stopped_lifecycle() {
    voip::VoipEventQueue queue;
    voip::VoipEventQueue::Reservation reservation;
    assert(queue.ReserveGuaranteed(Event(voip::EventType::incoming_call),
                                   &reservation));
    assert(reservation.IsGuaranteed());
    assert(reservation.Commit(Event(voip::EventType::incoming_call)));

    voip::Event terminal = Event(voip::EventType::call_state);
    terminal.destination_state = voip::CallState::terminated;
    assert(queue.Reserve(terminal, &reservation));
    assert(reservation.IsGuaranteed());
    assert(reservation.Commit(terminal));

    voip::Event registration = Event(voip::EventType::agent_snapshot);
    registration.agent_snapshot.registration =
        voip::RegistrationState::transport_failed;
    assert(queue.ReserveGuaranteed(registration, &reservation));
    assert(reservation.IsGuaranteed());
    assert(reservation.Commit(registration));

    voip::VoipEventQueue::Reservation stopped;
    voip::Event stopped_event = Event(voip::EventType::service_stopped);
    assert(queue.ReserveServiceStopped(&stopped));
    assert(stopped.IsPermanent());
    assert(stopped.Commit(stopped_event));
    assert(!queue.Publish(Event(voip::EventType::resource_snapshot)));

    voip::Event output{};
    while (queue.TryPop(&output)) {}
    assert(output.type == voip::EventType::service_stopped);
    assert(!queue.TryPop(&output));
}

void test_pressure_preserves_stopped_slot() {
    voip::VoipEventQueue queue;
    for (std::size_t index = 0; index < voip::VoipEventQueue::ordinary_capacity;
         ++index) {
        voip::Event event = Event(voip::EventType::operation_terminal,
                                  static_cast<std::uint8_t>(index));
        event.operation = static_cast<voip::OperationId>(index + 1);
        assert(queue.Publish(event));
    }
    voip::VoipEventQueue::Reservation incoming;
    voip::Event rejected = Event(voip::EventType::incoming_call);
    rejected.sequence = 77;
    assert(!queue.ReserveGuaranteed(rejected, &incoming));
    assert(rejected.sequence == 77);
    voip::VoipEventQueue::Reservation stopped;
    assert(queue.ReserveServiceStopped(&stopped));
    assert(queue.CommitServiceStopped(&stopped));
    voip::Event output{};
    for (std::size_t index = 0; index < voip::VoipEventQueue::ordinary_capacity;
         ++index) {
        assert(queue.TryPop(&output));
        assert(output.type == voip::EventType::operation_terminal);
    }
    assert(queue.TryPop(&output));
    assert(output.type == voip::EventType::service_stopped);
}

void test_reserved_event_cannot_publish_after_stopped() {
    voip::VoipEventQueue queue;
    voip::VoipEventQueue::Reservation ordinary;
    const voip::Event event = Event(voip::EventType::resource_snapshot);
    assert(queue.ReserveOrdinary(event, &ordinary));
    voip::VoipEventQueue::Reservation stopped;
    assert(queue.ReserveServiceStopped(&stopped));
    assert(stopped.Commit(Event(voip::EventType::service_stopped)));
    assert(!ordinary.Commit(event));
    assert(ordinary.Cancel());
    voip::Event output{};
    assert(queue.TryPop(&output));
    assert(output.type == voip::EventType::service_stopped);
    assert(!queue.TryPop(&output));
}

void test_coalesces_in_place_with_new_sequence() {
    voip::VoipEventQueue queue;
    voip::Event first = Event(voip::EventType::media_snapshot, 2, 4);
    first.media_snapshot.active = false;
    assert(queue.Publish(first));
    voip::Event old{};
    assert(queue.TryPeek(&old));
    voip::Event replacement = Event(voip::EventType::media_snapshot, 2, 4);
    replacement.media_snapshot.active = true;
    assert(queue.Publish(replacement));
    assert(queue.Size() == 1);
    voip::Event output{};
    assert(queue.TryPop(&output));
    assert(output.media_snapshot.active);
    assert(output.sequence > old.sequence);
}

void test_full_queue_still_accepts_matching_coalescible_replacement() {
    voip::VoipEventQueue queue;
    voip::Event retained = Event(voip::EventType::media_snapshot, 3, 8);
    assert(queue.Publish(retained));
    for (std::size_t index = 1; index < voip::VoipEventQueue::ordinary_capacity;
         ++index) {
        voip::Event event = Event(voip::EventType::operation_terminal,
                                  static_cast<std::uint8_t>(index));
        event.operation = static_cast<voip::OperationId>(index);
        assert(queue.Publish(event));
    }
    voip::Event replacement = Event(voip::EventType::media_snapshot, 3, 8);
    replacement.media_snapshot.active = true;
    assert(queue.Publish(replacement));
    assert(queue.Size() == voip::VoipEventQueue::ordinary_capacity);
    voip::Event output{};
    assert(queue.TryPop(&output));
    assert(output.type == voip::EventType::media_snapshot);
    assert(output.media_snapshot.active);
}

void test_sequences_are_nonzero_and_monotonic() {
    voip::VoipEventQueue queue;
    std::uint64_t prior = 0;
    for (unsigned index = 0; index < 100; ++index) {
        assert(queue.Publish(Event(voip::EventType::resource_snapshot,
                                   static_cast<std::uint8_t>(index))));
        voip::Event output{};
        assert(queue.TryPop(&output));
        assert(output.sequence != 0);
        assert(output.sequence > prior);
        prior = output.sequence;
    }
}

} // namespace

int main() {
    test_fifo_and_try_pop();
    test_wait_pop_timeout_and_wakeup();
    test_reservation_commit_cancel_exactly_once();
    test_guaranteed_classification_and_stopped_lifecycle();
    test_pressure_preserves_stopped_slot();
    test_coalesces_in_place_with_new_sequence();
    test_full_queue_still_accepts_matching_coalescible_replacement();
    test_reserved_event_cannot_publish_after_stopped();
    test_sequences_are_nonzero_and_monotonic();
    std::puts("VoipEventQueueTest PASSED");
    return 0;
}
