#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"

#include <cassert>
#include <cstring>

namespace voip::test {
class RecordingSink final : public PjsuaCallbackSink {
public:
    void OnRegistrationStarted(const PjsuaRegistrationRecord &record) noexcept override { ++started; last_started = record; }
    void OnRegistrationState(const PjsuaRegistrationRecord &record) noexcept override { ++state; last_state = record; }
    unsigned started = 0;
    unsigned state = 0;
    PjsuaRegistrationRecord last_started{};
    PjsuaRegistrationRecord last_state{};
};

void RunPjsuaCallbackRouterTests() {
    FakePjsuaApi fake;
    RecordingSink sink;
    PjsuaCallbackRouter first(sink, fake.Api());
    PjsuaCallbackRouter second(sink);
    pjsua_callback callbacks{};
    assert(first.Attach(&callbacks) == Error::ok);
    assert(second.Attach(&callbacks) == Error::invalid_state);
    char reason[] = "original reason";
    pjsip_regc_cbparam parameter{};
    parameter.status = PJ_EUNKNOWN;
    parameter.code = 503;
    parameter.reason = pj_str_t{reason, 15};
    parameter.expiration = 17;
    parameter.is_unreg = PJ_FALSE;
    pjsua_reg_info info{}; info.cbparam = &parameter; info.renew = PJ_TRUE;
    callbacks.on_reg_started2(0, &info);
    callbacks.on_reg_state2(0, &info);
    assert(sink.started == 1 && sink.state == 1);
    assert(sink.last_state.account == 0 && sink.last_state.native_status == PJ_EUNKNOWN &&
           sink.last_state.sip_status == 503 && sink.last_state.renew &&
           !sink.last_state.unregistration && sink.last_state.expiration == 17);
    reason[0] = 'X'; parameter.code = 599; parameter.expiration = 1;
    assert(sink.last_state.sip_status == 503 && sink.last_state.expiration == 17 &&
           sink.last_state.reason[0] == 'o');
    char long_reason[max_reason_length + 9];
    std::memset(long_reason, 'z', sizeof(long_reason));
    parameter.reason = pj_str_t{long_reason, static_cast<pj_ssize_t>(sizeof(long_reason))};
    parameter.code = 480;
    parameter.is_unreg = PJ_TRUE;
    parameter.expiration = 0;
    info.renew = PJ_FALSE;
    callbacks.on_reg_state2(0, &info);
    std::memset(long_reason, 'x', sizeof(long_reason));
    assert(sink.last_state.sip_status == 480 && sink.last_state.unregistration &&
           !sink.last_state.renew && sink.last_state.expiration == 0 &&
           sink.last_state.reason[max_reason_length] == '\0');
    for (std::size_t index = 0; index < max_reason_length; ++index)
        assert(sink.last_state.reason[index] == 'z');
    info.cbparam = nullptr;
    info.renew = PJ_TRUE;
    callbacks.on_reg_started2(0, &info);
    assert(sink.last_started.account == 0 && sink.last_started.native_status == PJ_SUCCESS &&
           sink.last_started.sip_status == 0 && sink.last_started.renew &&
           !sink.last_started.unregistration && sink.last_started.expiration == 0 &&
           sink.last_started.reason[0] == '\0');
    callbacks.on_incoming_call(0, 7, nullptr);
    assert(fake.SequenceEquals("answer,hangup"));
    assert(fake.AnswerCount() == 1 && fake.HangupCount() == 1);
    assert(fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    first.BeginQuiescence();
    callbacks.on_reg_state2(0, &info);
    assert(sink.state == 3);
    callbacks.on_incoming_call(0, 8, nullptr);
    assert(fake.AnswerCount() == 2 && fake.HangupCount() == 2);
    assert(fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    first.Detach();
    assert(second.Attach(&callbacks) == Error::invalid_state);
    callbacks.on_reg_started2(0, &info);
    assert(sink.started == 3);
    first.MarkNativeDestroyed();
    first.Detach();
    assert(second.Attach(&callbacks) == Error::ok);
    second.MarkNativeDestroyed();
    second.Detach();
}
} // namespace voip::test
