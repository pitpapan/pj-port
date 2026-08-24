#include <voip/FakeVoipBackend.hpp>

#include <zephyr/sys/printk.h>

#include <cstring>

namespace {

int failures;

#define CHECK(condition) do { if (!(condition)) { ++failures; } } while (false)

class TestObserver final : public voip::Observer {
public:
    unsigned registration_events{};
    unsigned incoming_events{};
    unsigned call_events{};
    unsigned media_events{};
    voip::CallInfo last_call{};

    void OnRegistrationState(voip::RegistrationState,
                             const voip::Status &) override {
        ++registration_events;
    }
    void OnIncomingCall(const voip::CallInfo &call) override {
        ++incoming_events;
        last_call = call;
    }
    void OnCallState(const voip::CallInfo &call,
                     const voip::Status &) override {
        ++call_events;
        last_call = call;
    }
    void OnMediaState(const voip::CallInfo &call,
                      const voip::Status &) override {
        ++media_events;
        last_call = call;
    }
};

void RunLifecycle(unsigned lifecycle) {
    voip::FakeVoipBackend backend;
    voip::VoipManager manager(backend);
    TestObserver observer;
    const voip::AccountConfig account{
        "sip:alice@127.0.0.1", "sip:127.0.0.1;transport=tcp",
        "alice", "phase1-secret", 300};

    CHECK(manager.RegisterAccount() == voip::Error::not_initialized);
    CHECK(manager.Initialize(&observer) == voip::Error::ok);
    CHECK(manager.Initialize(&observer) == voip::Error::invalid_state);
    CHECK(manager.ConfigureAccount(account) == voip::Error::ok);
    CHECK(manager.RegisterAccount() == voip::Error::ok);
    CHECK(manager.GetRegistrationState() == voip::RegistrationState::registered);

    CHECK(manager.StartOutgoingCall("sip:bob@127.0.0.1") == voip::Error::ok);
    CHECK(manager.StartOutgoingCall("sip:busy@127.0.0.1") == voip::Error::busy);
    CHECK(backend.EstablishCall(voip::Codec::pcmu) == voip::Error::ok);
    CHECK(manager.SetHeld(true) == voip::Error::ok);
    CHECK(manager.SetHeld(false) == voip::Error::ok);
    CHECK(manager.EndCall() == voip::Error::ok);

    CHECK(backend.InjectIncomingCall("sip:carol@127.0.0.1") == voip::Error::ok);
    CHECK(manager.AcceptCall() == voip::Error::ok);
    CHECK(backend.EstablishCall(voip::Codec::pcma) == voip::Error::ok);
    CHECK(manager.EndCall() == voip::Error::ok);
    CHECK(manager.UnregisterAccount() == voip::Error::ok);

    CHECK(observer.registration_events == 4);
    CHECK(observer.incoming_events == 1);
    CHECK(observer.media_events == 2);
    CHECK(std::strcmp(observer.last_call.remote_uri, "sip:carol@127.0.0.1") == 0);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(manager.Shutdown() == voip::Error::ok);
    CHECK(manager.StartOutgoingCall("sip:none@127.0.0.1") ==
          voip::Error::not_initialized);

    printk("[Phase 1] lifecycle %u facade/fake teardown: %s\n", lifecycle,
           failures == 0 ? "PASSED" : "FAILED");
}

} // namespace

int main() {
    printk("VoIP integration Phase 1 facade validation\n");
    for (unsigned lifecycle = 1; lifecycle <= 5; ++lifecycle) {
        RunLifecycle(lifecycle);
    }
    if (failures == 0) {
        printk("VOIP INTEGRATION PHASE 1 RESULT: PASSED (5 facade lifecycles)\n");
    } else {
        printk("VOIP INTEGRATION PHASE 1 RESULT: FAILED (%d checks)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
