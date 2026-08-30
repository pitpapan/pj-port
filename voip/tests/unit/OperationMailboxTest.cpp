#include "../../src/core/CommandMailbox.hpp"
#include "../../src/core/OperationTable.hpp"
#include "../../src/core/VoipCommand.hpp"
#include "../../src/core/VoipEventQueue.hpp"
#include "../../src/core/AgentRegistry.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <type_traits>

namespace {

static_assert(std::is_trivially_copyable<voip::VoipCommand>::value,
              "commands must be copied as fixed values");

voip::VoipCommand Dial(const char *uri, std::uint8_t slot = 0) {
    voip::VoipCommand command{};
    command.type = voip::CommandType::dial;
    command.dial.agent = {slot, 1};
    std::strncpy(command.dial.remote_uri, uri,
                 sizeof(command.dial.remote_uri) - 1);
    return command;
}

class Validator final : public voip::CommandHandleValidator {
public:
    explicit Validator(const voip::AgentRegistry *registry = nullptr) noexcept
        : registry_(registry) {}
    bool Validate(voip::AgentHandle handle) const noexcept override {
        return registry_ == nullptr ? handle.IsValid()
                                    : registry_->Resolve(handle) != nullptr;
    }
    bool Validate(voip::CallHandle handle) const noexcept override {
        return handle.generation == call_generation_ &&
               handle.slot == call_slot_ && handle.generation != 0;
    }
    void SetCall(voip::CallHandle handle) noexcept {
        call_slot_ = handle.slot;
        call_generation_ = handle.generation;
    }

private:
    const voip::AgentRegistry *registry_;
    std::uint8_t call_slot_ = 0xff;
    std::uint16_t call_generation_ = 0;
};

class Source final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override { return voip::Error::ok; }
};

class Sink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override { return voip::Error::ok; }
    void Flush() noexcept override {}
};

void test_copies_uri_and_reserves_one_terminal_event() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::VoipCommand command = Dial("sip:original@example.test");
    voip::OperationId operation = 0;
    Validator validator;
    assert(mailbox.Admit(operations, events, command, &operation, &validator) ==
           voip::Error::ok);
    std::strcpy(command.dial.remote_uri, "sip:mutated@example.test");
    voip::VoipCommand output{};
    assert(mailbox.TryPop(&output));
    assert(std::strcmp(output.dial.remote_uri, "sip:original@example.test") ==
           0);
    assert(output.operation == operation);
    assert(operations.ActiveCount() == 1);
    assert(events.Size() == 0);
    assert(operations.Complete(operation, voip::Error::ok));
    voip::Event event{};
    assert(events.TryPop(&event));
    assert(event.type == voip::EventType::operation_terminal);
    assert(event.operation == operation);
}

void test_dial_request_pointer_is_bounded_and_copied() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    char uri[] = "sip:pointer@example.test";
    voip::OperationId operation = 0;
    Validator validator;
    assert(mailbox.AdmitDial(operations, events, {0, 1}, {uri}, &operation,
                              &validator) == voip::Error::ok);
    std::strcpy(uri, "sip:changed@example.test");
    voip::VoipCommand output{};
    assert(mailbox.TryPop(&output));
    assert(std::strcmp(output.dial.remote_uri, "sip:pointer@example.test") ==
           0);
    char oversized[voip::max_uri_length + 2]{};
    for (std::size_t index = 0; index < voip::max_uri_length + 1; ++index)
        oversized[index] = 'x';
    assert(mailbox.AdmitDial(operations, events, {0, 1}, {oversized}, nullptr,
                              &validator) == voip::Error::invalid_argument);
}

void test_full_operation_table_rolls_back_without_consuming_id() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    std::array<voip::OperationId, voip::OperationTable::capacity> ids{};
    Validator validator;
    for (auto &id : ids) {
        assert(mailbox.Admit(operations, events, Dial("sip:a"), &id, &validator) ==
               voip::Error::ok);
    }
    voip::VoipCommand drained{};
    while (mailbox.TryPop(&drained)) {}
    voip::OperationId rejected = 99;
    assert(mailbox.Admit(operations, events, Dial("sip:full"), &rejected,
                         &validator) == voip::Error::resource_exhausted);
    assert(rejected == 0);
    for (const auto id : ids) assert(operations.Complete(id, voip::Error::ok));
    voip::OperationId next = 0;
    assert(mailbox.Admit(operations, events, Dial("sip:next"), &next, &validator) ==
           voip::Error::ok);
    assert(next == 17);
    assert(operations.Complete(next, voip::Error::ok));
    std::size_t terminal_count = 0;
    voip::Event output{};
    while (events.TryPop(&output)) {
        if (output.operation == next) ++terminal_count;
    }
    assert(terminal_count == 1);
}

void test_mailbox_full_rolls_back_operation_and_event() {
    voip::CommandMailbox mailbox;
    voip::OperationTable fill_operations;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    Validator validator;
    for (std::size_t index = 0; index < voip::CommandMailbox::capacity; ++index) {
        assert(mailbox.Admit(fill_operations, events, Dial("sip:a"), nullptr,
                             &validator) == voip::Error::ok);
    }
    assert(fill_operations.ActiveCount() == voip::CommandMailbox::capacity);
    const std::size_t events_before = events.Size();
    assert(mailbox.Admit(operations, events, Dial("sip:overflow"), nullptr,
                         &validator) == voip::Error::queue_full);
    assert(operations.ActiveCount() == 0);
    assert(events.Size() == events_before);
    voip::VoipCommand discarded{};
    while (mailbox.TryPop(&discarded)) {}
    for (std::size_t index = 0; index < voip::CommandMailbox::capacity;
         ++index) {
        assert(fill_operations.Complete(
            static_cast<voip::OperationId>(index + 1), voip::Error::ok));
    }
    voip::Event fill_event{};
    while (events.TryPop(&fill_event)) {}
    voip::OperationId operation = 0;
    assert(mailbox.Admit(operations, events, Dial("sip:after-rollback"),
                         &operation, &validator) == voip::Error::ok);
    assert(operation == 1);
    assert(operations.Complete(operation, voip::Error::ok));
    voip::Event output{};
    std::size_t terminal_count = 0;
    while (events.TryPop(&output))
        if (output.operation == operation) ++terminal_count;
    assert(terminal_count == 1);
}

void test_event_reservation_failure_rolls_back_without_id_consumption() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    for (std::size_t index = 0;
         index < voip::VoipEventQueue::ordinary_capacity; ++index) {
        voip::Event event{};
        event.type = voip::EventType::operation_terminal;
        event.operation = static_cast<voip::OperationId>(index + 1);
        assert(events.Publish(event));
    }
    Validator validator;
    assert(mailbox.Admit(operations, events, Dial("sip:no-event"), nullptr,
                         &validator) == voip::Error::queue_full);
    assert(operations.ActiveCount() == 0);
    voip::Event removed{};
    assert(events.TryPop(&removed));
    voip::OperationId operation = 0;
    assert(mailbox.Admit(operations, events, Dial("sip:recovered"),
                         &operation, &validator) == voip::Error::ok);
    assert(operation == 1);
    assert(operations.Complete(operation, voip::Error::ok));
    std::size_t terminal_count = 0;
    while (events.TryPop(&removed))
        if (removed.operation == operation) ++terminal_count;
    assert(terminal_count == 1);
}

void test_stale_handle_is_rejected_before_reservation() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId operation = 77;
    assert(mailbox.Admit(operations, events, Dial("sip:stale"), &operation,
                         nullptr) == voip::Error::invalid_handle);
    assert(operation == 0);
    assert(operations.ActiveCount() == 0);
    assert(mailbox.Size() == 0);
}

void test_real_registry_and_generation_validator_reject_stale_handles() {
    Source source;
    Sink sink;
    const voip::AgentConfig agent{
        {"sip:agent@example.test", "sip:example.test", "agent", "secret"},
        {&source, &sink}, false};
    const voip::ServiceConfig config{
        &agent, 1, 1000, 1000, {8000, 160, 1, voip::SampleFormat::signed_16},
        {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
    voip::AgentRegistry registry;
    assert(registry.Initialize(config) == voip::Error::ok);
    voip::AgentHandle stale{};
    assert(registry.GetAgentHandle(0, &stale) == voip::Error::ok);
    registry.Reset();
    assert(registry.Initialize(config) == voip::Error::ok);

    Validator validator(&registry);
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId operation = 99;
    voip::VoipCommand dial = Dial("sip:stale-agent");
    dial.dial.agent = stale;
    assert(mailbox.Admit(operations, events, dial, &operation, &validator) ==
           voip::Error::invalid_handle);
    assert(operation == 0);

    validator.SetCall({1, 2});
    voip::VoipCommand answer{};
    answer.type = voip::CommandType::answer;
    answer.answer.call = {1, 1};
    assert(mailbox.Admit(operations, events, answer, &operation, &validator) ==
           voip::Error::invalid_handle);
    answer.answer.call = {1, 2};
    assert(mailbox.Admit(operations, events, answer, &operation, &validator) ==
           voip::Error::ok);
    assert(operations.Complete(operation, voip::Error::ok));
}

void test_id_wrap_skips_live_collision() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations(UINT32_MAX);
    voip::VoipEventQueue events;
    voip::OperationId first = 0;
    voip::OperationId second = 0;
    Validator validator;
    assert(mailbox.Admit(operations, events, Dial("sip:first"), &first, &validator) ==
           voip::Error::ok);
    assert(first == UINT32_MAX);
    assert(mailbox.Admit(operations, events, Dial("sip:second"), &second,
                         &validator) == voip::Error::ok);
    assert(second == 1);
    assert(operations.Complete(first, voip::Error::ok));
    voip::OperationId third = 0;
    assert(mailbox.Admit(operations, events, Dial("sip:third"), &third, &validator) ==
           voip::Error::ok);
    assert(third == 2);
}

void test_empty_pop_preserves_output() {
    voip::CommandMailbox mailbox;
    voip::VoipCommand output = Dial("sip:unchanged");
    assert(!mailbox.TryPop(&output));
    assert(std::strcmp(output.dial.remote_uri, "sip:unchanged") == 0);
}

void test_reason_is_sanitized_at_all_boundaries() {
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId id = 0;
    assert(operations.Reserve(events, &id));
    assert(operations.AcceptAdmission(id));
    char exact[voip::max_reason_length + 1]{};
    for (std::size_t index = 0; index < voip::max_reason_length; ++index)
        exact[index] = 'e';
    assert(operations.Complete(id, voip::Error::internal_failure, 500,
                               exact));
    voip::Event output{};
    assert(events.TryPop(&output));
    assert(output.status.reason[voip::max_reason_length] == '\0');
    assert(output.status.reason[voip::max_reason_length - 1] == 'e');

    assert(operations.Reserve(events, &id));
    assert(operations.AcceptAdmission(id));
    char oversized[voip::max_reason_length + 2]{};
    for (std::size_t index = 0; index < voip::max_reason_length + 1; ++index)
        oversized[index] = 'o';
    assert(operations.Complete(id, voip::Error::internal_failure, 501,
                               oversized));
    assert(events.TryPop(&output));
    assert(output.status.reason[voip::max_reason_length] == '\0');
    assert(output.status.reason[voip::max_reason_length - 1] == 'o');

    assert(operations.Reserve(events, &id));
    assert(operations.AcceptAdmission(id));
    assert(operations.Complete(id, voip::Error::internal_failure, 502));
    assert(events.TryPop(&output));
    assert(output.status.reason[0] == '\0');
}

void test_failed_terminal_commit_retains_record_for_retry_or_rollback() {
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId id = 0;
    assert(operations.Reserve(events, &id));
    assert(operations.AcceptAdmission(id));
    voip::VoipEventQueue::Reservation stopped;
    assert(events.ReserveServiceStopped(&stopped));
    assert(events.CommitServiceStopped(&stopped));
    assert(!operations.Complete(id, voip::Error::ok));
    assert(operations.ActiveCount() == 1);
    assert(!operations.Complete(id, voip::Error::ok));
    assert(operations.RollbackAdmission(id) == false);
    assert(operations.Complete(id, voip::Error::cancelled) == false);
    assert(operations.ActiveCount() == 1);
}

void test_provisional_rollback_is_only_pre_acceptance_release() {
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId id = 0;
    assert(operations.Reserve(events, &id));
    assert(id == 1);
    assert(operations.RollbackAdmission(id));
    assert(operations.ActiveCount() == 0);
    assert(operations.Reserve(events, &id));
    assert(id == 1);
    assert(operations.AcceptAdmission(id));
    assert(!operations.RollbackAdmission(id));
    assert(operations.ActiveCount() == 1);
    assert(operations.Complete(id, voip::Error::ok));
}

void test_old_id_cannot_complete_reused_record() {
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::OperationId old_id = 0;
    assert(operations.Reserve(events, &old_id));
    assert(operations.AcceptAdmission(old_id));
    assert(operations.Complete(old_id, voip::Error::ok));
    voip::Event event{};
    assert(events.TryPop(&event));
    voip::OperationId new_id = 0;
    assert(operations.Reserve(events, &new_id));
    assert(operations.AcceptAdmission(new_id));
    assert(new_id != old_id);
    assert(!operations.Complete(old_id, voip::Error::internal_failure));
    assert(operations.ActiveCount() == 1);
    assert(operations.Complete(new_id, voip::Error::ok));
}

void test_shutdown_is_the_only_handleless_admission() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    voip::VoipCommand shutdown{};
    shutdown.type = voip::CommandType::shutdown;
    assert(mailbox.Admit(operations, events, shutdown) ==
           voip::Error::invalid_argument);
    assert(operations.ActiveCount() == 0);

    voip::CommandMailbox internal_mailbox;
    assert(internal_mailbox.TryPushShutdown());
    assert(!internal_mailbox.TryPushShutdown());
    voip::VoipCommand output{};
    assert(internal_mailbox.TryPop(&output));
    assert(output.type == voip::CommandType::shutdown);
    assert(output.operation == 0);
    assert(!internal_mailbox.TryPushShutdown());
}

void test_multi_producer_fifo_has_no_duplicates() {
    voip::CommandMailbox mailbox;
    voip::OperationTable operations;
    voip::VoipEventQueue events;
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t per_producer = 4;
    std::array<std::thread, producer_count> producers;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers[producer] = std::thread([&, producer]() {
            for (std::size_t index = 0; index < per_producer; ++index) {
                voip::OperationId operation = 0;
                Validator validator;
                assert(mailbox.Admit(operations, events,
                                     Dial("sip:producer"), &operation, &validator) ==
                       voip::Error::ok);
            }
        });
    }
    for (auto &producer : producers) producer.join();
    voip::VoipCommand command{};
    std::size_t count = 0;
    while (mailbox.TryPop(&command)) {
        assert(command.operation == count + 1);
        ++count;
    }
    assert(count == producer_count * per_producer);
}

} // namespace

int main() {
    test_copies_uri_and_reserves_one_terminal_event();
    test_dial_request_pointer_is_bounded_and_copied();
    test_full_operation_table_rolls_back_without_consuming_id();
    test_mailbox_full_rolls_back_operation_and_event();
    test_event_reservation_failure_rolls_back_without_id_consumption();
    test_stale_handle_is_rejected_before_reservation();
    test_real_registry_and_generation_validator_reject_stale_handles();
    test_id_wrap_skips_live_collision();
    test_empty_pop_preserves_output();
    test_reason_is_sanitized_at_all_boundaries();
    test_failed_terminal_commit_retains_record_for_retry_or_rollback();
    test_provisional_rollback_is_only_pre_acceptance_release();
    test_old_id_cannot_complete_reused_record();
    test_shutdown_is_the_only_handleless_admission();
    test_multi_producer_fifo_has_no_duplicates();
    std::puts("OperationMailboxTest PASSED");
    return 0;
}
