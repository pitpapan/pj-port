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
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

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
    return {agents.data(), static_cast<std::uint8_t>(agents.size()), 2, 2,
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

    voip::OperationId cancel_operation = 0;
    assert(service.Cancel(calls[6], &cancel_operation) == voip::Error::ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
