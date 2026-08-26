#include <voip/VoipService.hpp>

#include <cassert>
#include <cstring>

namespace {

class FakeBackend final : public voip::Backend {
public:
    voip::Observer *observer = nullptr;
    bool initialized = false;
    bool account_configured = false;
    unsigned calls = 0;

    voip::Error Initialize(voip::Observer *value) override {
        observer = value;
        initialized = true;
        return voip::Error::ok;
    }
    voip::Error Shutdown() override {
        initialized = false;
        observer = nullptr;
        return voip::Error::ok;
    }
    voip::Error ConfigureAccount(const voip::AccountConfig &) override {
        account_configured = true;
        return voip::Error::ok;
    }
    voip::Error RegisterAccount() override {
        voip::Status status{};
        status.error = voip::Error::ok;
        observer->OnRegistrationState(voip::RegistrationState::registered,
                                      status);
        return voip::Error::ok;
    }
    voip::Error UnregisterAccount() override { return voip::Error::ok; }
    voip::Error StartOutgoingCall(const char *) override {
        ++calls;
        return voip::Error::ok;
    }
    voip::Error AcceptCall() override { return voip::Error::ok; }
    voip::Error RejectCall(std::uint16_t) override { return voip::Error::ok; }
    voip::Error EndCall() override { return voip::Error::ok; }
    voip::Error SetHeld(bool) override { return voip::Error::ok; }
    voip::RegistrationState GetRegistrationState() const override {
        return voip::RegistrationState::registered;
    }
    voip::CallInfo GetCallInfo() const override { return voip::CallInfo{}; }
};

class Events final : public voip::EventHandler {
public:
    unsigned count = 0;
    voip::Event last{};

    void OnEvent(const voip::Event &event) noexcept override {
        ++count;
        last = event;
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

void test_service_admits_one_lifecycle() {
    FakeBackend backend;
    voip::VoipService service(backend);
    Events events;
    voip::ServiceConfig config{
        reinterpret_cast<k_work_q *>(1), 4, 4};
    assert(service.Initialize(config, &events) == voip::Error::ok);

    const voip::AccountConfig account{
        "sip:alice@127.0.0.1", "sip:127.0.0.1;transport=tcp",
        "alice", "secret", 300};
    voip::AccountHandle account_handle{};
    assert(service.AddAccount(account, &account_handle) == voip::Error::ok);

    voip::OperationId registration_operation = 0;
    assert(service.SetRegistration(account_handle, true,
                                   &registration_operation) == voip::Error::ok);
    assert(registration_operation != 0);
    assert(events.count == 1);
    assert(events.last.type == voip::EventType::account_state);
    assert(events.last.operation == registration_operation);

    Source source;
    Sink sink;
    const voip::CallConfig call_config{
        "sip:bob@127.0.0.1", voip::Codec::pcmu, &source, &sink};
    voip::CallHandle call_handle{};
    voip::OperationId dial_operation = 0;
    assert(service.Dial(account_handle, call_config, &call_handle,
                        &dial_operation) == voip::Error::ok);
    assert(call_handle.IsValid());
    assert(dial_operation != registration_operation);
    assert(backend.calls == 1);
    assert(call_handle.IsValid());
    assert(service.RemoveAccount(account_handle) == voip::Error::busy);
    assert(service.Hangup(call_handle, &dial_operation) == voip::Error::ok);
    assert(service.Shutdown() == voip::Error::ok);
}

void test_invalid_handles_are_rejected() {
    FakeBackend backend;
    voip::VoipService service(backend);
    voip::AccountHandle account{};
    voip::OperationId operation = 0;
    assert(service.SetRegistration(account, true, &operation) ==
           voip::Error::not_initialized);
}

} // namespace

int main() {
    test_service_admits_one_lifecycle();
    test_invalid_handles_are_rejected();
    return 0;
}
