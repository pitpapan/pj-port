#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>
#include <zephyr/sys/printk.h>

namespace voip::test {
namespace {
class Source final : public PcmSource {
public:
    PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; }
    Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; }
};
class Sink final : public PcmSink {
public:
    PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; }
    Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; }
    void Flush() noexcept override {}
};
struct Fixture {
    Source source{}; Sink sink{};
    AgentConfig agent{{"sip:a@example.test", "sip:registrar.example.test", "a", "p"},
                      {&source, &sink}, true};
    ServiceConfig config{&agent, 1, 1000, 5000,
                         {8000, 160, 1, SampleFormat::signed_16},
                         {SignalingSecurity::none, MediaSecurity::none}};
    AgentRegistry registry{};
    Fixture() noexcept { assert(registry.Initialize(config) == Error::ok); }
};

void OpenCall(const PjsuaApi &api, pjsua_call_id *id) {
    pjsua_call_setting setting{};
    api.call_setting_default(&setting);
    char uri_text[] = "sip:remote@example.test";
    pj_str_t uri{uri_text, static_cast<pj_ssize_t>(sizeof(uri_text) - 1)};
    assert(api.call_make_call(0, &uri, &setting, nullptr, nullptr, id) == PJ_SUCCESS);
}

pj_status_t DisconnectedInfo(pjsua_call_id id, pjsua_call_info *info) {
    if (info == nullptr) return PJ_EINVAL;
    *info = {};
    info->id = id;
    info->state = PJSIP_INV_STATE_DISCONNECTED;
    return PJ_SUCCESS;
}
}

void RunPjsuaIncomingAdmissionTests() {
    Fixture fixture;
    FakePjsuaApi fake;
    fake.Reset();
    PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(fixture.registry, 4) == Error::ok);
    PjsuaCallManager calls(accounts, fake.Api());
    pjsua_call_id id = PJSUA_INVALID_ID;
    OpenCall(fake.Api(), &id);
    calls.OnIncomingCall(2, id);
    RuntimeNotification notification{};
    assert(calls.TryGetNotification(&notification));
    CallHandle logical{3, 1};

    // Removing the native 180 handshake or binding the wrong provisional
    // token leaves either the context unbound or a non-180 native sequence.
    assert(calls.AdmitIncoming(logical, notification.agent, notification.token) == Error::ok);
    assert(fake.CallUserData(id) != nullptr);
    assert(fake.SequenceEquals(
        "add,call-defaults,make-call,get-call-data,set-call-data,get-call-info,"
        "get-call-data,"
        "call-defaults,answer-180"));

    fake.Reset();
    PjsuaAccountManager failed_accounts(fake.Api());
    assert(failed_accounts.Initialize(fixture.registry, 4) == Error::ok);
    PjsuaCallManager failed(failed_accounts, fake.Api());
    OpenCall(fake.Api(), &id);
    failed.OnIncomingCall(2, id);
    assert(failed.TryGetNotification(&notification));
    fake.FailCall(FakePjsuaApi::CallEntry::answer2, PJ_EUNKNOWN);
    assert(failed.AdmitIncoming(logical, notification.agent, notification.token) ==
           Error::signaling_failed);
    // Failed 180 retains the exact native user-data route until DISCONNECTED.
    assert(fake.CallUserData(id) != nullptr && failed.Count() == 1);
    fake.ClearCallFailures();

    // Replacing the copied terminal record with direct callback handling
    // would leave the actor's logical call live after a remote CANCEL.
    PjsuaApi cancel_api = fake.Api();
    cancel_api.call_get_info = &DisconnectedInfo;
    PjsuaCallManager cancelled(failed_accounts, cancel_api);
    OpenCall(cancel_api, &id);
    cancelled.OnIncomingCall(2, id);
    assert(cancelled.TryGetNotification(&notification));
    assert(cancelled.AdmitIncoming(logical, notification.agent, notification.token) == Error::ok);
    cancelled.OnCallState(id);
    assert(cancelled.TryGetNotification(&notification));
    assert(notification.type == RuntimeNotification::Type::call_finished);
    assert(cancelled.Count() == 0);
    printk("PjsuaIncomingAdmissionTest PASSED\n");
}
} // namespace voip::test
