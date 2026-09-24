#include "FakePjsuaApi.hpp"
#include "../../src/core/AgentRegistry.hpp"
#include "../../src/pjsua/PjsuaAccountManager.hpp"
#include "../../src/pjsua/PjsuaCallManager.hpp"

#include <cassert>

namespace voip::test {
namespace {
class Source final : public PcmSource { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Read(std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } };
class Sink final : public PcmSink { public: PcmFormat Format() const noexcept override { return {8000, 160, 1, SampleFormat::signed_16}; } Error Write(const std::int16_t *, std::size_t, std::uint64_t) noexcept override { return Error::ok; } void Flush() noexcept override {} };
struct Fixture {
    Source source; Sink sink;
    AgentConfig agent{{"sip:zero@example.test", "sip:registrar.example.test", "user", "password"}, {&source, &sink}, true};
    ServiceConfig config{&agent, 1, 1000, 5000, {8000, 160, 1, SampleFormat::signed_16}, {SignalingSecurity::none, MediaSecurity::none}};
    AgentRegistry registry;
    Fixture() noexcept { assert(registry.Initialize(config) == Error::ok); }
};
unsigned hold_calls = 0, resume_calls = 0, answer_calls = 0;
pjsua_call_info control_info[7]{};
pj_status_t ControlInfo(pjsua_call_id id, pjsua_call_info *info) {
    if (id < 0 || id >= 7 || info == nullptr) return PJ_EINVAL;
    *info = control_info[id];
    return PJ_SUCCESS;
}
pj_status_t Hold(pjsua_call_id id, unsigned options, const pjsua_msg_data *data) {
    assert(id >= 0 && options == 0 && data == nullptr);
    ++hold_calls; return PJ_SUCCESS;
}
pj_status_t Resume(pjsua_call_id id, const pjsua_call_setting *setting, const pjsua_msg_data *data) {
    assert(id >= 0 && setting != nullptr && (setting->flag & PJSUA_CALL_UNHOLD) != 0 && data == nullptr);
    ++resume_calls; return PJ_SUCCESS;
}
pj_status_t Answer(pjsua_call_id id, const pjsua_call_setting *setting, unsigned status,
                    const pj_str_t *, const pjsua_msg_data *) {
    assert(id >= 0 && setting != nullptr && status == 200);
    ++answer_calls; return PJ_SUCCESS;
}
}

void RunPjsuaCallControlTests() {
    hold_calls = 0; resume_calls = 0; answer_calls = 0;
    for (auto &info : control_info) info = {};
    Fixture fixture; FakePjsuaApi fake; fake.Reset();
    PjsuaAccountManager accounts(fake.Api());
    assert(accounts.Initialize(fixture.registry, 4) == Error::ok);
    auto api = fake.Api();
    api.call_get_info = ControlInfo;
    api.call_set_hold2 = Hold; api.call_reinvite2 = Resume; api.call_answer2 = Answer;
    PjsuaCallManager calls(accounts, api);
    AgentHandle agent{}; assert(fixture.registry.GetAgentHandle(0, &agent) == Error::ok);

    CallHandle outgoing{0, 1}; std::uint32_t token = 0;
    assert(calls.PromoteOutgoing(outgoing, agent, "sip:peer@example.test", &token) == Error::ok);
    // Removing the 487 compatibility status must fail this command/state mapping.
    assert(calls.Cancel(token) == Error::ok);
    assert(fake.HangupCount() == 1 && fake.HangupStatus() == 487);
    assert(calls.Hangup(token) == Error::ok);
    assert(fake.HangupCount() == 2 && fake.HangupStatus() == 0);
    control_info[0].state = PJSIP_INV_STATE_CONFIRMED;
    calls.OnCallState(0);
    RuntimeNotification media_result{};
    assert(calls.TryGetNotification(&media_result) &&
           media_result.type == RuntimeNotification::Type::call_accepted);
    assert(calls.SetHeld(token, true) == Error::ok);
    control_info[0].media_status = PJSUA_CALL_MEDIA_LOCAL_HOLD;
    control_info[0].media_dir = PJMEDIA_DIR_ENCODING;
    calls.OnCallMediaState(0);
    assert(calls.TryGetNotification(&media_result) &&
           media_result.type == RuntimeNotification::Type::call_held);
    assert(calls.SetHeld(token, false) == Error::ok);
    control_info[0].media_status = PJSUA_CALL_MEDIA_ACTIVE;
    control_info[0].media_dir = PJMEDIA_DIR_ENCODING_DECODING;
    calls.OnCallMediaState(0);
    assert(calls.TryGetNotification(&media_result) &&
           media_result.type == RuntimeNotification::Type::call_resumed);
    assert(hold_calls == 1 && resume_calls == 1);
    assert(calls.Answer(token) == Error::ok && answer_calls == 1);

    // Stale tokens and out-of-range SIP reject statuses must make no native call.
    assert(calls.Answer(token + 1) == Error::invalid_handle);
    assert(calls.Reject(token, 399) == Error::invalid_argument);
    assert(calls.Reject(token, 700) == Error::invalid_argument);
    assert(calls.Reject(token, 486) == Error::ok);
    assert(fake.AnswerCount() == 1 && fake.AnswerStatus() == 486);
    assert(calls.Reject(token, 400) == Error::ok && fake.AnswerStatus() == 400);
    assert(calls.Reject(token, 699) == Error::ok && fake.AnswerStatus() == 699);
    fake.FailCall(FakePjsuaApi::CallEntry::hangup, PJ_EUNKNOWN);
    assert(calls.Hangup(token) == Error::signaling_failed);
    fake.ClearCallFailures();
    // Native dispatch trusts actor policy and only validates native ownership.
    pjsua_call_setting incoming_setting{};
    api.call_setting_default(&incoming_setting);
    char remote[] = "sip:incoming@example.test";
    pj_str_t destination{remote, sizeof(remote) - 1};
    pjsua_call_id incoming_id = PJSUA_INVALID_ID;
    assert(api.call_make_call(2, &destination, &incoming_setting, nullptr, nullptr,
                             &incoming_id) == PJ_SUCCESS);
    calls.OnIncomingCall(2, incoming_id);
    RuntimeNotification incoming{};
    assert(calls.TryGetNotification(&incoming));
    assert(calls.Cancel(incoming.token) == Error::ok && fake.HangupStatus() == 487);
    // Accepted controls must retain the callback route and bounded context.
    assert(calls.Count() == 2 && fake.CallUserData(0) != nullptr);
    int foreign = 0;
    assert(api.call_set_user_data(0, &foreign) == PJ_SUCCESS);
    const auto answers = fake.AnswerCount();
    const auto hangups = fake.HangupCount();
    assert(calls.Answer(token) == Error::invalid_handle);
    assert(calls.Reject(token, 480) == Error::invalid_handle);
    assert(calls.Cancel(token) == Error::invalid_handle);
    assert(calls.Hangup(token) == Error::invalid_handle);
    assert(calls.SetHeld(token, true) == Error::invalid_handle);
    assert(calls.SetHeld(token, false) == Error::invalid_handle);
    assert(fake.AnswerCount() == answers && fake.HangupCount() == hangups);
    assert(hold_calls == 1 && resume_calls == 1 && answer_calls == 1);
    assert(fake.CallUserData(0) == &foreign && calls.Count() == 2);
}
} // namespace voip::test
