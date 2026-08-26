#include <voip/FakeVoipBackend.hpp>
#include <voip/VoipService.hpp>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <cstring>

namespace {

K_THREAD_STACK_DEFINE(event_stack, 2048);
struct k_work_q event_queue;
int failures;

#define CHECK(condition) do { if (!(condition)) ++failures; } while (false)

class Events final : public voip::EventHandler {
public:
    unsigned registration_events{};
    unsigned call_events{};

    void OnEvent(const voip::Event &event) noexcept override {
        if (event.type == voip::EventType::account_state)
            ++registration_events;
        if (event.type == voip::EventType::call_state)
            ++call_events;
    }
};

class Source final : public voip::PcmSource {
public:
    voip::Error Read(std::int16_t *, std::size_t,
                     std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

class Sink final : public voip::PcmSink {
public:
    voip::Error Write(const std::int16_t *, std::size_t,
                      std::uint64_t) noexcept override {
        return voip::Error::ok;
    }
};

} // namespace

int main() {
    k_work_queue_start(&event_queue, event_stack,
                       K_THREAD_STACK_SIZEOF(event_stack), 5, nullptr);

    voip::FakeVoipBackend backend;
    voip::VoipService service(backend);
    Events events;
    const voip::ServiceConfig config{&event_queue, 8, 8};
    CHECK(service.Initialize(config, &events) == voip::Error::ok);

    const voip::AccountConfig account{
        "sip:alice@127.0.0.1", "sip:127.0.0.1;transport=tcp",
        "alice", "sdk-contract-secret", 300};
    voip::AccountHandle account_handle{};
    CHECK(service.AddAccount(account, &account_handle) == voip::Error::ok);

    voip::OperationId operation = 0;
    CHECK(service.SetRegistration(account_handle, true, &operation) ==
          voip::Error::ok);
    k_sleep(K_MSEC(20));
    CHECK(events.registration_events == 2);

    Source source;
    Sink sink;
    const voip::CallConfig call{
        "sip:bob@127.0.0.1", voip::Codec::pcmu, &source, &sink};
    voip::CallHandle call_handle{};
    CHECK(service.Dial(account_handle, call, &call_handle, &operation) ==
          voip::Error::ok);
    k_sleep(K_MSEC(20));
    CHECK(events.call_events == 1);
    CHECK(service.RemoveAccount(account_handle) == voip::Error::busy);
    CHECK(service.Hangup(call_handle, &operation) == voip::Error::ok);
    k_sleep(K_MSEC(20));
    CHECK(service.Shutdown() == voip::Error::ok);

    printk("VOIP SDK CONTRACT RESULT: %s\n",
           failures == 0 ? "PASSED" : "FAILED");
    return failures == 0 ? 0 : 1;
}
