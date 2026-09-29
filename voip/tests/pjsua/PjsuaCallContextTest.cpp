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

// Defaults to DISCONNECTED so the pressure scenario is unchanged; the flag
// lets the same manager then observe a fresh INVITE on a recycled call id.
bool recycled_call_disconnected = true;
pj_status_t DisconnectedInfo(pjsua_call_id id, pjsua_call_info *info) {
    if (info == nullptr) return PJ_EINVAL;
    std::memset(info, 0, sizeof(*info));
    info->id = id;
    info->state = recycled_call_disconnected ? PJSIP_INV_STATE_DISCONNECTED
                                             : PJSIP_INV_STATE_INCOMING;
    return PJ_SUCCESS;
}

void AssertCallResourceBaseline(PjsuaCallManager &calls, const FakePjsuaApi &fake) {
    RuntimeNotification notification{};
    assert(calls.Count() == 0 && !calls.TryGetNotification(&notification));
    assert(fake.ActiveCallCount() == 7);
    for (pjsua_call_id id = 0; id < 7; ++id)
        assert(fake.CallUserData(id) == nullptr);
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
    AssertCallResourceBaseline(calls, fake);

    // Breaking this requires treating an unowned account like capacity
    // pressure instead of rejecting it as an unknown SIP destination.
    calls.OnIncomingCall(99, 0);
    assert(fake.AnswerStatus() == 404 && fake.HangupStatus() == 404);
    AssertCallResourceBaseline(calls, fake);

    // A foreign native owner is never overwritten or cleared by incoming
    // routing. Removing the pre-bind check makes this assertion fail.
    int foreign_prebind{};
    assert(fake.Api().call_set_user_data(0, &foreign_prebind) == PJ_SUCCESS);
    calls.OnIncomingCall(2, 0);
    assert(calls.Count() == 0 && fake.CallUserData(0) == &foreign_prebind &&
           fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    assert(fake.Api().call_set_user_data(0, nullptr) == PJ_SUCCESS);
    AssertCallResourceBaseline(calls, fake);

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
    AssertCallResourceBaseline(calls, fake);
    calls.OnIncomingCall(2, 6);
    assert(calls.TryGetNotification(&notification));
    assert(notification.token != 0 && notification.token != tokens[0]);
    calls.Reset();
    AssertCallResourceBaseline(calls, fake);

    // A foreign native user-data pointer must not route a callback into an
    // owned context, and callbacks after Reset are similarly ignored.
    int foreign_postbind{};
    calls.OnIncomingCall(2, 6);
    assert(fake.Api().call_set_user_data(6, &foreign_postbind) == PJ_SUCCESS);
    calls.OnCallState(6);
    assert(calls.Count() == 1);
    calls.Reset();
    assert(fake.CallUserData(6) == &foreign_postbind);
    assert(fake.Api().call_set_user_data(6, nullptr) == PJ_SUCCESS);
    calls.OnCallState(6);
    calls.OnCallMediaState(6);
    AssertCallResourceBaseline(calls, fake);

    // Context capacity is exactly seven. An eighth incoming callback is
    // declined after the bounded table is full.
    for (pjsua_call_id id : ids) calls.OnIncomingCall(2, id);
    calls.OnIncomingCall(2, 7);
    assert(calls.Count() == 7 && fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    calls.Reset();
    AssertCallResourceBaseline(calls, fake);

    // Disconnected contexts retain their copied admission and finish records
    // until the actor drains them; their storage cannot be reused behind the
    // backlog even though native user data is already unreachable.
    PjsuaApi pressure_api = fake.Api();
    pressure_api.call_get_info = &DisconnectedInfo;
    PjsuaCallManager pressure(accounts, pressure_api);
    for (pjsua_call_id id : ids) pressure.OnIncomingCall(2, id);
    for (pjsua_call_id id : ids) pressure.OnCallState(id);
    assert(pressure.Count() == 7);
    for (pjsua_call_id id : ids) assert(fake.CallUserData(id) == nullptr);
    pressure.OnIncomingCall(2, 6);
    assert(pressure.Count() == 7 && fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    std::uint32_t retained_tokens[7]{};
    for (std::size_t index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::incoming_call);
        retained_tokens[index] = notification.token;
    }
    for (std::size_t index = 0; index < 7; ++index) {
        assert(pressure.TryGetNotification(&notification));
        assert(notification.type == RuntimeNotification::Type::call_finished &&
               notification.token == retained_tokens[index]);
    }
    AssertCallResourceBaseline(pressure, fake);

    // PJSUA recycles a pjsua_call_id as soon as the call is freed, while a
    // disconnected context stays reserved until the actor drains its copied
    // finish record. The retained context must not keep claiming that id, or
    // the next INVITE carrying it is declined with 486 although six contexts
    // are free. Reuses the pressure manager so no extra call-manager frame
    // overlaps this one's main-stack budget.
    const unsigned answer_before = fake.AnswerStatus();
    const unsigned hangup_before = fake.HangupStatus();
    recycled_call_disconnected = true;
    pressure.OnIncomingCall(2, 3);
    assert(pressure.Count() == 1 && fake.CallUserData(3) != nullptr);
    pressure.OnCallState(3);
    // Native ownership is unreachable, but the context is still reserved.
    assert(pressure.Count() == 1 && fake.CallUserData(3) == nullptr);
    recycled_call_disconnected = false;
    pressure.OnIncomingCall(2, 3);
    assert(pressure.Count() == 2 && fake.CallUserData(3) != nullptr);
    assert(pressure.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::incoming_call);
    const std::uint32_t recycled_first = notification.token;
    assert(pressure.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::call_finished &&
           notification.token == recycled_first);
    assert(pressure.TryGetNotification(&notification) &&
           notification.type == RuntimeNotification::Type::incoming_call &&
           notification.token != recycled_first);
    assert(!pressure.TryGetNotification(&notification));
    assert(pressure.Count() == 1);
    // The recycled-id INVITE was admitted, so no new decline was issued.
    assert(fake.AnswerStatus() == answer_before && fake.HangupStatus() == hangup_before);
    pressure.Reset();
    AssertCallResourceBaseline(pressure, fake);
    printk("PjsuaCallContextTest PASSED\n");
}
} // namespace voip::test
