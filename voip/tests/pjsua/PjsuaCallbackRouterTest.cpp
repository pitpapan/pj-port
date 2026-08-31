#include "FakePjsuaApi.hpp"
#include "../../src/pjsua/PjsuaCallbackRouter.hpp"

#include <cassert>

namespace voip::test {
class RecordingSink final : public PjsuaCallbackSink {
public:
    void OnRegistrationStarted(const PjsuaRegistrationRecord &) noexcept override { ++started; }
    void OnRegistrationState(const PjsuaRegistrationRecord &) noexcept override { ++state; }
    unsigned started = 0;
    unsigned state = 0;
};

void RunPjsuaCallbackRouterTests() {
    FakePjsuaApi fake;
    RecordingSink sink;
    PjsuaCallbackRouter first(sink, fake.Api());
    PjsuaCallbackRouter second(sink);
    pjsua_callback callbacks{};
    assert(first.Attach(&callbacks) == Error::ok);
    assert(second.Attach(&callbacks) == Error::invalid_state);
    pjsua_reg_info info{};
    callbacks.on_reg_started2(0, &info);
    callbacks.on_reg_state2(0, &info);
    assert(sink.started == 1 && sink.state == 1);
    callbacks.on_incoming_call(0, 7, nullptr);
    assert(fake.SequenceEquals("answer,hangup"));
    assert(fake.AnswerCount() == 1 && fake.HangupCount() == 1);
    assert(fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    first.BeginQuiescence();
    callbacks.on_reg_state2(0, &info);
    assert(sink.state == 2);
    callbacks.on_incoming_call(0, 8, nullptr);
    assert(fake.AnswerCount() == 2 && fake.HangupCount() == 2);
    assert(fake.AnswerStatus() == 486 && fake.HangupStatus() == 486);
    first.Detach();
    assert(second.Attach(&callbacks) == Error::invalid_state);
    callbacks.on_reg_started2(0, &info);
    assert(sink.started == 2);
    first.MarkNativeDestroyed();
    first.Detach();
    assert(second.Attach(&callbacks) == Error::ok);
    second.MarkNativeDestroyed();
    second.Detach();
}
} // namespace voip::test
