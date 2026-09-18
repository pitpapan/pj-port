#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>
#include <cstring>
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
    Source sources[5]{}; Sink sinks[5]{}; AgentConfig agents[5]{};
    ServiceConfig config{agents, 5, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16},
                         {SignalingSecurity::none, MediaSecurity::none}};
    Fixture() noexcept {
        const char *identities[] = {"sip:zero@example.test", "sip:one@example.test",
                                    "sip:two@example.test", "sip:three@example.test",
                                    "sip:four@example.test"};
        for (std::size_t i = 0; i < 5; ++i)
            agents[i] = {{identities[i], "sip:registrar.example.test", "agent", "password"},
                         {&sources[i], &sinks[i]}, true};
    }
};

void OpenNativeCalls(const PjsuaApi &api) {
    pjsua_call_setting setting{};
    api.call_setting_default(&setting);
    char uri_text[] = "sip:remote@example.test";
    pj_str_t uri{uri_text, static_cast<pj_ssize_t>(sizeof(uri_text) - 1)};
    for (unsigned index = 0; index < 7; ++index) {
        pjsua_call_id id = PJSUA_INVALID_ID;
        assert(api.call_make_call(0, &uri, &setting, nullptr, nullptr, &id) == PJ_SUCCESS);
        assert(id == static_cast<pjsua_call_id>(index));
    }
}

pj_status_t DisconnectedInfo(pjsua_call_id id, pjsua_call_info *info) {
    if (info == nullptr) return PJ_EINVAL;
    std::memset(info, 0, sizeof(*info));
    info->id = id;
    info->state = PJSIP_INV_STATE_DISCONNECTED;
    return PJ_SUCCESS;
}
}

void RunPjsuaCallContextTests() {
    Fixture fixture; AgentRegistry registry;
    assert(registry.Initialize(fixture.config) == Error::ok);
    FakePjsuaApi fake; fake.Reset();
    PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(registry, 4) == Error::ok);
    PjsuaCallManager calls(accounts, fake.Api());
    OpenNativeCalls(fake.Api());

    // Breaking this requires treating an unowned account like capacity
    // pressure instead of rejecting it as an unknown SIP destination.
    calls.OnIncomingCall(99, 0);
    assert(fake.AnswerStatus() == 404 && fake.HangupStatus() == 404);

    // Breaking this requires mapping a native ID to its allocation slot rather
    // than retaining the callback's actual ID.
    const pjsua_call_id ids[] = {6, 2, 5, 0, 4, 1, 3};
    for (pjsua_call_id id : ids) calls.OnIncomingCall(2, id);
    assert(calls.Count() == 7);
    RuntimeNotification notification{};
    std::uint32_t tokens[7]{};
    for (std::size_t index = 0; index < 7; ++index) {
        assert(calls.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::incoming_call);
        assert(notification.agent.slot == 0 && notification.token != 0);
        tokens[index] = notification.token;
    }
    assert(!calls.TryGetNotification(&notification));
    calls.OnIncomingCall(2, 6); // duplicate native ID must be declined.
    assert(calls.Count() == 7 && fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);

    calls.Reset();
    assert(calls.Count() == 0 && !calls.TryGetNotification(&notification));
    calls.OnIncomingCall(2, 6);
    assert(calls.TryGetNotification(&notification));
    assert(notification.token != 0 && notification.token != tokens[0]);

    // A foreign native user-data pointer must not route a callback into an
    // owned context, and callbacks after Reset are similarly ignored.
    int foreign{};
    assert(fake.Api().call_set_user_data(6, &foreign) == PJ_SUCCESS);
    calls.OnCallState(6);
    assert(calls.Count() == 1);
    calls.Reset();
    calls.OnCallState(6);
    calls.OnCallMediaState(6);
    assert(calls.Count() == 0 && !calls.TryGetNotification(&notification));

    // Context capacity is exactly seven. An eighth incoming callback is
    // declined after the bounded table is full.
    for (pjsua_call_id id : ids) calls.OnIncomingCall(2, id);
    calls.OnIncomingCall(2, 7);
    assert(calls.Count() == 7 && fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    calls.Reset();
    assert(calls.Count() == 0 && fake.Api().call_get_user_data(6) == nullptr);

    // The copied notification ring remains bounded even when native contexts
    // are released before the actor gets a chance to drain it.
    PjsuaApi pressure_api = fake.Api();
    pressure_api.call_get_info = &DisconnectedInfo;
    PjsuaCallManager pressure(accounts, pressure_api);
    for (pjsua_call_id id : ids) pressure.OnIncomingCall(2, id);
    for (pjsua_call_id id : ids) pressure.OnCallState(id);
    assert(pressure.Count() == 0);
    pressure.OnIncomingCall(2, 6);
    assert(pressure.Count() == 0 && fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    pressure.Reset();
    printk("PjsuaCallContextTest PASSED\n");
}
} // namespace voip::test
