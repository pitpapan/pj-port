#include <voip/VoipService.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <limits>

namespace {

std::atomic<std::size_t> allocations{0};

void *Allocate(std::size_t size) {
    void *memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
}

} // namespace

void *operator new(std::size_t size) { return Allocate(size); }
void *operator new[](std::size_t size) { return Allocate(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try { return Allocate(size); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    try { return Allocate(size); } catch (...) { return nullptr; }
}
void *AllocateAligned(std::size_t size, std::size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<std::size_t>::max() - alignment + 1)
        throw std::bad_alloc();
    const std::size_t rounded =
        ((size + alignment - 1) / alignment) * alignment;
    void *memory = std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
    if (memory == nullptr) throw std::bad_alloc();
    allocations.fetch_add(1, std::memory_order_relaxed);
    return memory;
}
void *operator new(std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
    return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
    try { return AllocateAligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
    try { return AllocateAligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}
void operator delete[](void *memory, std::size_t,
                       std::align_val_t) noexcept { std::free(memory); }

namespace {

class Source final : public voip::PcmSource {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

class Sink final : public voip::PcmSink {
public:
    voip::PcmFormat Format() const noexcept override {
        return {8000, 160, 1, voip::SampleFormat::signed_16};
    }
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
    void Flush() noexcept override {}
};

voip::ServiceConfig MakeConfig(std::array<voip::AgentConfig, 5> &agents,
                               std::array<Source, 5> &sources,
                               std::array<Sink, 5> &sinks) {
    static constexpr const char *const identities[5] = {
        "sip:heap0@example.test", "sip:heap1@example.test",
        "sip:heap2@example.test", "sip:heap3@example.test",
        "sip:heap4@example.test",
    };
    for (std::size_t i = 0; i < agents.size(); ++i) {
        agents[i] = {{identities[i], "sip:example.test", "user", "password"},
                     {&sources[i], &sinks[i]}, true};
    }
    return {agents.data(), static_cast<std::uint8_t>(agents.size()), 1000, 1000,
            {8000, 160, 1, voip::SampleFormat::signed_16},
            {voip::SignalingSecurity::none, voip::MediaSecurity::none}};
}

bool GetCallEvent(voip::VoipService &service, voip::OperationId operation,
                  voip::CallHandle *call) {
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::call_state &&
            event.operation == operation) {
            *call = event.call;
            return true;
        }
    }
    return false;
}

void Drain(voip::VoipService &service) {
    voip::Event event{};
    while (service.TryGetEvent(&event) == voip::Error::ok) {}
}

void test_no_heap_after_successful_initialization() {
    std::array<Source, 5> sources;
    std::array<Sink, 5> sinks;
    std::array<voip::AgentConfig, 5> agents{};
    const voip::ServiceConfig config = MakeConfig(agents, sources, sinks);
    voip::VoipService service;
    assert(service.Initialize(config) == voip::Error::ok);
    Drain(service);
    allocations.store(0, std::memory_order_relaxed);

    std::array<voip::AgentHandle, 5> handles{};
    for (std::uint8_t i = 0; i < handles.size(); ++i)
        assert(service.GetAgentHandle(i, &handles[i]) == voip::Error::ok);

    std::array<voip::CallHandle, 7> calls{};
    std::array<voip::OperationId, 7> dial_operations{};
    for (std::size_t i = 0; i < calls.size(); ++i) {
        assert(service.Dial(handles[i % handles.size()], {"sip:peer@example.test"},
                            &dial_operations[i]) == voip::Error::ok);
        assert(GetCallEvent(service, dial_operations[i], &calls[i]));
    }

    const voip::ResourceSnapshot full = service.GetResourceSnapshot();
    assert(full.active_calls == 7);
    assert(full.promoted_calls == 2);
    assert(full.queued_calls == 5);
    assert(full.available_fifo_entries == 0);
    assert(full.available_logical_calls == 0);
    assert(full.available_promoted_calls == 0);

    // Two state changes are submitted before polling. The fixed queue must
    // retain only the newest matching call-state snapshot for this handle.
    voip::OperationId hold_operation = 0;
    voip::OperationId resume_operation = 0;
    assert(service.SetHeld(calls[0], true, &hold_operation) == voip::Error::ok);
    bool hold_snapshot_seen = false;
    for (unsigned attempt = 0; attempt < 200 && !hold_snapshot_seen; ++attempt) {
        voip::CallSnapshot snapshot{};
        hold_snapshot_seen = service.GetCallSnapshot(calls[0], &snapshot) ==
                                 voip::Error::ok &&
                             snapshot.state == voip::CallState::hold;
        if (!hold_snapshot_seen)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(hold_snapshot_seen);
    assert(service.SetHeld(calls[0], false, &resume_operation) == voip::Error::ok);
    bool resume_snapshot_seen = false;
    for (unsigned attempt = 0; attempt < 200 && !resume_snapshot_seen; ++attempt) {
        voip::CallSnapshot snapshot{};
        resume_snapshot_seen = service.GetCallSnapshot(calls[0], &snapshot) ==
                                   voip::Error::ok &&
                               snapshot.state == voip::CallState::established;
        if (!resume_snapshot_seen)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(resume_snapshot_seen);
    voip::CallSnapshot resumed{};
    assert(service.GetCallSnapshot(calls[0], &resumed) == voip::Error::ok);
    assert(resumed.state == voip::CallState::established);

    // Cancel one FIFO entry and observe its operation completion. A different
    // FIFO entry then times out, publishes its terminal result, and is stale.
    voip::OperationId cancel_operation = 0;
    assert(service.Cancel(calls[6], &cancel_operation) == voip::Error::ok);
    bool cancel_seen = false;
    bool hold_seen = false;
    bool resume_seen = false;
    unsigned matching_call_states = 0;
    voip::Event event{};
    for (unsigned attempt = 0; attempt < 200 && !cancel_seen; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        if (event.type == voip::EventType::call_state &&
            event.call.slot == calls[0].slot &&
            event.call.generation == calls[0].generation) {
            ++matching_call_states;
            hold_seen |= event.destination_state == voip::CallState::hold;
            resume_seen |= event.destination_state == voip::CallState::established &&
                           event.transition == voip::CallTransition::resume;
        }
        if (event.type == voip::EventType::operation_terminal &&
            event.operation == cancel_operation) {
            cancel_seen = event.status.error == voip::Error::cancelled;
        }
    }
    assert(cancel_seen);
    assert(matching_call_states == 1);
    assert(!hold_seen);
    assert(resume_seen);

    const voip::OperationId timeout_operation = dial_operations[5];
    bool timeout_seen = false;
    for (unsigned attempt = 0; attempt < 150 && !timeout_seen; ++attempt) {
        if (service.WaitForEvent(&event, 20) != voip::Error::ok) continue;
        timeout_seen = event.type == voip::EventType::operation_terminal &&
                       event.operation == timeout_operation &&
                       event.status.error == voip::Error::timed_out;
    }
    assert(timeout_seen);
    voip::CallSnapshot timed_out{};
    assert(service.GetCallSnapshot(calls[5], &timed_out) ==
           voip::Error::invalid_handle);

    Drain(service);

    assert(service.Shutdown() == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
    Drain(service);
    assert(allocations.load(std::memory_order_relaxed) == 0);
}

} // namespace

int main() {
    test_no_heap_after_successful_initialization();
    std::puts("NoHeapAfterInitTest PASSED");
    return 0;
}
